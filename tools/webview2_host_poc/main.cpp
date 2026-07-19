// osfui-webview2-host-poc — Phase 1 game stand-in for the out-of-process
// WebView2 host. Plays Starfield's role end to end WITHOUT the game:
//
//   - mirrors osfui_webview2_host.exe to a real path (%LOCALAPPDATA%\OSFUI\
//     bin\poc\), the same move the plugin must make out of the MO2 VFS
//   - broker-launches it OUT of this process's tree (Explorer -> Task
//     Scheduler -> direct), records which method worked and the host's parent
//   - serves the control pipe, drives init/navigate/resize/focus/shutdown
//   - opens the host's NT-handle shared texture ring + shared fences on a
//     D3D12 device and composites the view over an animated background —
//     the exact consumer the game-side D3D12 compositor will be
//   - proves cross-process SetParent keyboard input with a synthetic typing
//     round-trip, 20 focus/hide cycles, and a resize
//   - verifies clean mutual shutdown and hunts for orphaned processes
//
// Run with no arguments for the automated gate sequence (exit code 0 = all
// gates passed; per-gate lines on stdout). --interactive keeps the window
// open for manual poking (F10 toggles the overlay like the game).

#include "Wv2BrokerLaunch.h"
#include "Wv2Pipe.h"
#include "Wv2Protocol.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <format>
#include <functional>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <Windows.h>
#include <windowsx.h>
#include <ShlObj.h>
#include <TlHelp32.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <nlohmann/json.hpp>

using Microsoft::WRL::ComPtr;
using nlohmann::json;
using namespace osfui::wv2;

namespace
{
	// ---------------------------------------------------------------- util --

	std::wstring ToWide(std::string_view a_text)
	{
		if (a_text.empty()) return {};
		const auto size = ::MultiByteToWideChar(CP_UTF8, 0,
			a_text.data(), static_cast<int>(a_text.size()), nullptr, 0);
		std::wstring out(static_cast<std::size_t>(size), L'\0');
		::MultiByteToWideChar(CP_UTF8, 0, a_text.data(),
			static_cast<int>(a_text.size()), out.data(), size);
		return out;
	}

	std::string ToUtf8(std::wstring_view a_text)
	{
		if (a_text.empty()) return {};
		const auto size = ::WideCharToMultiByte(CP_UTF8, 0, a_text.data(),
			static_cast<int>(a_text.size()), nullptr, 0, nullptr, nullptr);
		std::string out(static_cast<std::size_t>(size), '\0');
		::WideCharToMultiByte(CP_UTF8, 0, a_text.data(), static_cast<int>(a_text.size()),
			out.data(), size, nullptr, nullptr);
		return out;
	}

	void Print(const std::string& a_line)
	{
		std::printf("%s\n", a_line.c_str());
		std::fflush(stdout);
	}

	struct GateResults
	{
		int  failures{ 0 };
		void Report(const char* a_gate, bool a_pass, const std::string& a_detail)
		{
			Print(std::format("GATE {:<10} : {} — {}", a_gate, a_pass ? "PASS" : "FAIL", a_detail));
			if (!a_pass) ++failures;
		}
	};

	std::filesystem::path LocalAppData()
	{
		PWSTR value = nullptr;
		std::filesystem::path out;
		if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &value))) {
			out = value;
			::CoTaskMemFree(value);
		}
		return out;
	}

	std::filesystem::path OwnDirectory()
	{
		std::array<wchar_t, 32768> buffer{};
		const auto length = ::GetModuleFileNameW(nullptr, buffer.data(),
			static_cast<DWORD>(buffer.size()));
		return std::filesystem::path(std::wstring_view(buffer.data(), length)).parent_path();
	}

	std::filesystem::path FindViewsRoot()
	{
		const auto fromCwd = std::filesystem::current_path() / "data" / "OSFUI" / "views";
		if (std::filesystem::exists(fromCwd / "osfui" / "settings" / "index.html")) {
			return fromCwd;
		}
		auto cursor = OwnDirectory();
		for (int depth = 0; depth < 8 && !cursor.empty(); ++depth) {
			const auto candidate = cursor / "data" / "OSFUI" / "views";
			if (std::filesystem::exists(candidate / "osfui" / "settings" / "index.html")) {
				return candidate;
			}
			cursor = cursor.parent_path();
		}
		return fromCwd;
	}

	// pid -> ppid map + name check, for the out-of-tree proof and orphan hunt.
	struct ProcessInfo { DWORD pid; DWORD ppid; std::wstring name; };
	std::vector<ProcessInfo> SnapshotProcesses()
	{
		std::vector<ProcessInfo> out;
		const HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snap == INVALID_HANDLE_VALUE) return out;
		PROCESSENTRY32W entry{ sizeof(entry) };
		if (::Process32FirstW(snap, &entry)) {
			do {
				out.push_back({ entry.th32ProcessID, entry.th32ParentProcessID, entry.szExeFile });
			} while (::Process32NextW(snap, &entry));
		}
		::CloseHandle(snap);
		return out;
	}

	std::wstring ProcessName(DWORD a_pid)
	{
		for (const auto& p : SnapshotProcesses()) {
			if (p.pid == a_pid) return p.name;
		}
		return L"<gone>";
	}

	// Every process whose ancestor chain reaches a_root (by stale-tolerant
	// ppid walk) and whose name matches.
	std::vector<DWORD> DescendantsNamed(DWORD a_root, const std::wstring& a_name)
	{
		const auto procs = SnapshotProcesses();
		std::vector<DWORD> out;
		for (const auto& p : procs) {
			if (_wcsicmp(p.name.c_str(), a_name.c_str()) != 0) continue;
			DWORD cursor = p.ppid;
			for (int depth = 0; depth < 16 && cursor != 0; ++depth) {
				if (cursor == a_root) {
					out.push_back(p.pid);
					break;
				}
				DWORD next = 0;
				for (const auto& q : procs) {
					if (q.pid == cursor) { next = q.ppid; break; }
				}
				cursor = next;
			}
		}
		return out;
	}

	// ------------------------------------------------------------ D3D12 -----

	constexpr const char* kVertexShader = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut main(uint id : SV_VertexID) {
    VSOut o;
    o.uv  = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv.x * 2.0 - 1.0, 1.0 - o.uv.y * 2.0, 0.0, 1.0);
    return o;
}
)";
	constexpr const char* kPixelShader = R"(
Texture2D    gTex : register(t0);
SamplerState gSmp : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return gTex.Sample(gSmp, uv);
}
)";

	constexpr UINT kBackBuffers = 3;

	struct D3D12Client
	{
		ComPtr<ID3D12Device>              device;
		ComPtr<ID3D12CommandQueue>        queue;
		ComPtr<IDXGISwapChain3>           swap;
		ComPtr<ID3D12DescriptorHeap>      rtvHeap;
		ComPtr<ID3D12DescriptorHeap>      srvHeap;  // kRingSlots SRVs
		ComPtr<ID3D12RootSignature>       rootSig;
		ComPtr<ID3D12PipelineState>       pso;
		ComPtr<ID3D12Fence>               frameFence;
		HANDLE                            frameFenceEvent{ nullptr };
		std::uint64_t                     nextFenceValue{ 1 };
		UINT                              rtvStride{ 0 };
		UINT                              srvStride{ 0 };
		std::uint32_t                     width{ 0 }, height{ 0 };
		struct FrameCtx
		{
			ComPtr<ID3D12CommandAllocator> allocator;
			std::uint64_t                  fenceValue{ 0 };
		};
		std::array<FrameCtx, kBackBuffers> frames{};
		ComPtr<ID3D12GraphicsCommandList>  list;

		// shared ring, opened from the host's duplicated handles
		std::array<ComPtr<ID3D12Resource>, kRingSlots> slots{};
		std::uint32_t       ringWidth{ 0 }, ringHeight{ 0 };
		ComPtr<ID3D12Fence> produceFence, consumeFence;

		bool Initialize(HWND a_window, std::uint32_t a_width, std::uint32_t a_height)
		{
			width = a_width;
			height = a_height;
			if (FAILED(::D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
					IID_PPV_ARGS(&device)))) {
				Print("d3d12: D3D12CreateDevice failed");
				return false;
			}
			D3D12_COMMAND_QUEUE_DESC qd{};
			qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
			if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) return false;

			ComPtr<IDXGIFactory4> factory;
			if (FAILED(::CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
			DXGI_SWAP_CHAIN_DESC1 sd{};
			sd.Width = width;
			sd.Height = height;
			sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			sd.SampleDesc.Count = 1;
			sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			sd.BufferCount = kBackBuffers;
			sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
			ComPtr<IDXGISwapChain1> swap1;
			if (FAILED(factory->CreateSwapChainForHwnd(queue.Get(), a_window, &sd,
					nullptr, nullptr, &swap1)) ||
				FAILED(swap1.As(&swap))) {
				Print("d3d12: CreateSwapChainForHwnd failed");
				return false;
			}
			factory->MakeWindowAssociation(a_window, DXGI_MWA_NO_ALT_ENTER);

			D3D12_DESCRIPTOR_HEAP_DESC rd{};
			rd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
			rd.NumDescriptors = kBackBuffers;
			if (FAILED(device->CreateDescriptorHeap(&rd, IID_PPV_ARGS(&rtvHeap)))) return false;
			rtvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
			D3D12_DESCRIPTOR_HEAP_DESC sdh{};
			sdh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			sdh.NumDescriptors = kRingSlots;
			sdh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			if (FAILED(device->CreateDescriptorHeap(&sdh, IID_PPV_ARGS(&srvHeap)))) return false;
			srvStride = device->GetDescriptorHandleIncrementSize(
				D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			CreateBackBufferViews();

			for (auto& f : frames) {
				if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
						IID_PPV_ARGS(&f.allocator)))) {
					return false;
				}
			}
			if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
					frames[0].allocator.Get(), nullptr, IID_PPV_ARGS(&list)))) {
				return false;
			}
			list->Close();
			if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
					IID_PPV_ARGS(&frameFence)))) {
				return false;
			}
			frameFenceEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
			return CreatePipeline();
		}

		void CreateBackBufferViews()
		{
			for (UINT i = 0; i < kBackBuffers; ++i) {
				ComPtr<ID3D12Resource> buffer;
				if (SUCCEEDED(swap->GetBuffer(i, IID_PPV_ARGS(&buffer)))) {
					const D3D12_CPU_DESCRIPTOR_HANDLE rtv{
						rtvHeap->GetCPUDescriptorHandleForHeapStart().ptr +
						static_cast<SIZE_T>(i) * rtvStride
					};
					device->CreateRenderTargetView(buffer.Get(), nullptr, rtv);
				}
			}
		}

		bool CreatePipeline()
		{
			D3D12_DESCRIPTOR_RANGE range{};
			range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			range.NumDescriptors = 1;
			D3D12_ROOT_PARAMETER param{};
			param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			param.DescriptorTable.NumDescriptorRanges = 1;
			param.DescriptorTable.pDescriptorRanges = &range;
			param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			D3D12_STATIC_SAMPLER_DESC sampler{};
			sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			sampler.AddressU = sampler.AddressV = sampler.AddressW =
				D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			sampler.MaxLOD = D3D12_FLOAT32_MAX;
			D3D12_ROOT_SIGNATURE_DESC rs{};
			rs.NumParameters = 1;
			rs.pParameters = &param;
			rs.NumStaticSamplers = 1;
			rs.pStaticSamplers = &sampler;
			ComPtr<ID3DBlob> blob, error;
			if (FAILED(::D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1,
					&blob, &error))) {
				return false;
			}
			if (FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(),
					blob->GetBufferSize(), IID_PPV_ARGS(&rootSig)))) {
				return false;
			}
			ComPtr<ID3DBlob> vs, ps, err;
			if (FAILED(::D3DCompile(kVertexShader, std::strlen(kVertexShader), nullptr,
					nullptr, nullptr, "main", "vs_5_0", 0, 0, &vs, &err)) ||
				FAILED(::D3DCompile(kPixelShader, std::strlen(kPixelShader), nullptr,
					nullptr, nullptr, "main", "ps_5_0", 0, 0, &ps, &err))) {
				return false;
			}
			D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
			pd.pRootSignature = rootSig.Get();
			pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
			pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
			pd.SampleMask = UINT_MAX;
			pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
			pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			pd.NumRenderTargets = 1;
			pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
			pd.SampleDesc.Count = 1;
			auto& rt = pd.BlendState.RenderTarget[0];
			rt.BlendEnable = TRUE;
			rt.SrcBlend = D3D12_BLEND_ONE;
			rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
			rt.BlendOp = D3D12_BLEND_OP_ADD;
			rt.SrcBlendAlpha = D3D12_BLEND_ONE;
			rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
			rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
			rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			return SUCCEEDED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)));
		}

		void WaitIdle()
		{
			const auto value = nextFenceValue++;
			queue->Signal(frameFence.Get(), value);
			if (frameFence->GetCompletedValue() < value) {
				frameFence->SetEventOnCompletion(value, frameFenceEvent);
				::WaitForSingleObject(frameFenceEvent, 2000);
			}
		}

		void Resize(std::uint32_t a_width, std::uint32_t a_height)
		{
			if (!swap || (a_width == width && a_height == height) || !a_width || !a_height) {
				return;
			}
			WaitIdle();
			width = a_width;
			height = a_height;
			swap->ResizeBuffers(kBackBuffers, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
			CreateBackBufferViews();
		}

		void ReleaseRing()
		{
			WaitIdle();
			for (auto& s : slots) s.Reset();
			ringWidth = ringHeight = 0;
		}

		// Opens the duplicated handles the host announced. Fences persist
		// across ring recreations, so only (re)open them when absent.
		bool OpenRing(const json& a_msg, std::string& a_detail)
		{
			ReleaseRing();
			const auto& handleList = a_msg.at("slots");
			for (std::size_t i = 0; i < kRingSlots && i < handleList.size(); ++i) {
				const auto handle = reinterpret_cast<HANDLE>(
					static_cast<std::uintptr_t>(handleList[i].get<std::uint64_t>()));
				const auto hr = device->OpenSharedHandle(handle, IID_PPV_ARGS(&slots[i]));
				::CloseHandle(handle);
				if (FAILED(hr)) {
					a_detail = std::format("OpenSharedHandle(texture {}) failed 0x{:08X}",
						i, static_cast<unsigned>(hr));
					return false;
				}
			}
			if (!produceFence) {
				const auto handle = reinterpret_cast<HANDLE>(
					static_cast<std::uintptr_t>(a_msg.value("produceFence", 0ull)));
				const auto hr = device->OpenSharedHandle(handle, IID_PPV_ARGS(&produceFence));
				::CloseHandle(handle);
				if (FAILED(hr)) {
					a_detail = std::format("OpenSharedHandle(produceFence) failed 0x{:08X}",
						static_cast<unsigned>(hr));
					return false;
				}
			} else if (const auto h = a_msg.value("produceFence", 0ull)) {
				::CloseHandle(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(h)));
			}
			if (!consumeFence) {
				const auto handle = reinterpret_cast<HANDLE>(
					static_cast<std::uintptr_t>(a_msg.value("consumeFence", 0ull)));
				const auto hr = device->OpenSharedHandle(handle, IID_PPV_ARGS(&consumeFence));
				::CloseHandle(handle);
				if (FAILED(hr)) {
					a_detail = std::format("OpenSharedHandle(consumeFence) failed 0x{:08X}",
						static_cast<unsigned>(hr));
					return false;
				}
			} else if (const auto h = a_msg.value("consumeFence", 0ull)) {
				::CloseHandle(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(h)));
			}
			ringWidth = a_msg.value("width", 0u);
			ringHeight = a_msg.value("height", 0u);
			for (std::uint32_t i = 0; i < kRingSlots; ++i) {
				D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
				sv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
				sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				sv.Texture2D.MipLevels = 1;
				const D3D12_CPU_DESCRIPTOR_HANDLE srv{
					srvHeap->GetCPUDescriptorHandleForHeapStart().ptr +
					static_cast<SIZE_T>(i) * srvStride
				};
				device->CreateShaderResourceView(slots[i].Get(), &sv, srv);
			}
			a_detail = std::format("{}x{} ring opened on D3D12 (keyedMutex={})",
				ringWidth, ringHeight, a_msg.value("keyedMutex", false));
			return true;
		}

		// Reads back one shared slot through a copy + readback buffer and
		// counts non-zero-alpha pixels — the "the pixels really made it across
		// two processes" proof.
		std::uint64_t CountOpaquePixels(std::uint32_t a_slot, std::uint64_t a_serial)
		{
			if (!slots[a_slot]) return 0;
			WaitIdle();  // frames[0].allocator is reused below; drain in-flight work
			if (produceFence) {
				// The host's D3D11 copy for this serial must be GPU-visible
				// before the cross-device copy below reads the slot.
				queue->Wait(produceFence.Get(), a_serial);
			}
			const auto rowPitch = (ringWidth * 4u + 255u) & ~255u;
			const auto size = static_cast<std::uint64_t>(rowPitch) * ringHeight;
			D3D12_HEAP_PROPERTIES props{};
			props.Type = D3D12_HEAP_TYPE_READBACK;
			D3D12_RESOURCE_DESC desc{};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			desc.Width = size;
			desc.Height = 1;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.SampleDesc.Count = 1;
			desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			ComPtr<ID3D12Resource> readback;
			if (FAILED(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
					D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) {
				return 0;
			}
			auto& frame = frames[0];
			frame.allocator->Reset();
			list->Reset(frame.allocator.Get(), nullptr);
			D3D12_TEXTURE_COPY_LOCATION src{};
			src.pResource = slots[a_slot].Get();
			src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			D3D12_TEXTURE_COPY_LOCATION dst{};
			dst.pResource = readback.Get();
			dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			dst.PlacedFootprint.Footprint.Width = ringWidth;
			dst.PlacedFootprint.Footprint.Height = ringHeight;
			dst.PlacedFootprint.Footprint.Depth = 1;
			dst.PlacedFootprint.Footprint.RowPitch = rowPitch;
			list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
			list->Close();
			ID3D12CommandList* lists[]{ list.Get() };
			queue->ExecuteCommandLists(1, lists);
			WaitIdle();
			std::uint8_t* mapped = nullptr;
			if (FAILED(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) {
				return 0;
			}
			std::uint64_t count = 0;
			for (std::uint32_t y = 0; y < ringHeight; ++y) {
				const auto* row = mapped + static_cast<std::size_t>(y) * rowPitch;
				for (std::uint32_t x = 0; x < ringWidth; ++x) {
					if (row[x * 4 + 3] != 0) ++count;
				}
			}
			readback->Unmap(0, nullptr);
			return count;
		}

		// One present: animated background, then the overlay slot (if any).
		void Render(double a_seconds, bool a_overlayVisible,
			std::optional<std::pair<std::uint32_t, std::uint64_t>> a_frame)
		{
			const auto backIndex = swap->GetCurrentBackBufferIndex();
			auto& frame = frames[backIndex];
			if (frame.fenceValue != 0 && frameFence->GetCompletedValue() < frame.fenceValue) {
				frameFence->SetEventOnCompletion(frame.fenceValue, frameFenceEvent);
				::WaitForSingleObject(frameFenceEvent, 1000);
			}
			ComPtr<ID3D12Resource> backbuffer;
			if (FAILED(swap->GetBuffer(backIndex, IID_PPV_ARGS(&backbuffer)))) return;

			if (a_frame && produceFence) {
				// Same-queue wait: the draw below must not sample the slot
				// before the host's copy for this serial completed.
				queue->Wait(produceFence.Get(), a_frame->second);
			}

			frame.allocator->Reset();
			list->Reset(frame.allocator.Get(), pso.Get());
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = backbuffer.Get();
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
			barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
			list->ResourceBarrier(1, &barrier);

			const D3D12_CPU_DESCRIPTOR_HANDLE rtv{
				rtvHeap->GetCPUDescriptorHandleForHeapStart().ptr +
				static_cast<SIZE_T>(backIndex) * rtvStride
			};
			const float pulse = 0.5f + 0.5f * static_cast<float>(std::sin(a_seconds * 2.0));
			const float clear[4] = { 0.08f + 0.10f * pulse, 0.10f, 0.16f - 0.05f * pulse, 1.0f };
			list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
			list->ClearRenderTargetView(rtv, clear, 0, nullptr);

			if (a_overlayVisible && a_frame && slots[a_frame->first]) {
				ID3D12DescriptorHeap* heaps[]{ srvHeap.Get() };
				list->SetDescriptorHeaps(1, heaps);
				list->SetGraphicsRootSignature(rootSig.Get());
				const D3D12_GPU_DESCRIPTOR_HANDLE srv{
					srvHeap->GetGPUDescriptorHandleForHeapStart().ptr +
					static_cast<UINT64>(a_frame->first) * srvStride
				};
				list->SetGraphicsRootDescriptorTable(0, srv);
				D3D12_VIEWPORT vp{ 0, 0, static_cast<float>(width),
					static_cast<float>(height), 0, 1 };
				D3D12_RECT scissor{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
				list->RSSetViewports(1, &vp);
				list->RSSetScissorRects(1, &scissor);
				list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				list->DrawInstanced(3, 1, 0, 0);
			}

			barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
			barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
			list->ResourceBarrier(1, &barrier);
			list->Close();
			ID3D12CommandList* lists[]{ list.Get() };
			queue->ExecuteCommandLists(1, lists);
			if (a_frame && consumeFence) {
				// The host may only rewrite this slot once our read finished.
				queue->Signal(consumeFence.Get(), a_frame->second);
			}
			frame.fenceValue = nextFenceValue++;
			queue->Signal(frameFence.Get(), frame.fenceValue);
			swap->Present(1, 0);
		}
	};

	// -------------------------------------------------------------- app -----

	struct ClientApp
	{
		HWND        window{ nullptr };
		D3D12Client d3d;
		Pipe        pipe;
		std::thread pipeThread;
		HANDLE      hostProcess{ nullptr };
		DWORD       hostPid{ 0 };
		LaunchResult launch;
		bool        interactive{ false };
		std::atomic_bool running{ true };
		std::atomic_bool pipeDead{ false };

		std::mutex       inboundMutex;
		std::deque<json> inbound;

		// state driven by inbound messages (main thread)
		bool  helloSeen{ false }, readySeen{ false }, domReady{ false };
		json  pendingTextures;
		bool  texturesDirty{ false };
		std::string texturesDetail;
		bool  texturesOpened{ false };
		std::optional<std::pair<std::uint32_t, std::uint64_t>> latestFrame;
		std::uint64_t framesSeen{ 0 };
		std::uint64_t lastEvalId{ 0 };
		std::unordered_map<std::uint64_t, std::string> evalResults;  // id -> result
		bool overlayVisible{ true };
		std::uint32_t lastCursorId{ 0 };
		std::vector<std::string> acceleratorLog;
		bool byeSeen{ false };

		std::filesystem::path viewsRoot, mirrorDir, userDataDir;

		// ----- pipe plumbing -----

		void Send(const json& a_msg) { pipe.WriteMessage(a_msg.dump()); }

		void PipeReader()
		{
			std::string payload;
			while (pipe.ReadMessage(payload)) {
				json parsed = json::parse(payload, nullptr, false);
				if (parsed.is_discarded()) continue;
				const std::string type = parsed.value("type", "");
				if (type == "frame") {
					// Hot path: track latest + CPU-ack skipped frames so the
					// host's ring never waits on a hidden/lagging consumer.
					std::scoped_lock lock(inboundMutex);
					inbound.push_back(std::move(parsed));
				} else {
					std::scoped_lock lock(inboundMutex);
					inbound.push_back(std::move(parsed));
				}
			}
			pipeDead.store(true);
		}

		void HandleInbound(const json& a_msg)
		{
			const std::string type = a_msg.value("type", "");
			if (type == "hello") {
				helloSeen = true;
				hostPid = a_msg.value("pid", 0u);
				Print(std::format("client: hello from host pid {} (runtime {}, protocol {})",
					hostPid, a_msg.value("runtimeVersion", "?"),
					a_msg.value("protocolVersion", 0u)));
				if (hostPid) {
					hostProcess = ::OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, hostPid);
				}
			} else if (type == "ready") {
				readySeen = true;
			} else if (type == "textures") {
				pendingTextures = a_msg;
				texturesDirty = true;
			} else if (type == "frame") {
				const auto w = a_msg.value("width", 0u);
				const auto h = a_msg.value("height", 0u);
				const auto serial = a_msg.value("serial", 0ull);
				if (w == d3d.ringWidth && h == d3d.ringHeight && d3d.slots[0]) {
					latestFrame = { a_msg.value("slot", 0u), serial };
					++framesSeen;
					if (!overlayVisible && d3d.consumeFence) {
						d3d.consumeFence->Signal(serial);  // CPU-ack skipped frames
					}
				} else if (d3d.consumeFence) {
					d3d.consumeFence->Signal(serial);  // stale ring; ack + drop
				}
			} else if (type == "domReady") {
				domReady = true;
				Print("client: DOM ready");
			} else if (type == "loadEvent") {
				Print(std::format("client: load {} url={} code={}",
					a_msg.value("failed", false) ? "FAILED" : "ok",
					a_msg.value("url", ""), a_msg.value("code", 0)));
			} else if (type == "evalResult") {
				evalResults[a_msg.value("id", 0ull)] = a_msg.value("result", "");
			} else if (type == "cursor") {
				lastCursorId = a_msg.value("id", 0u);
			} else if (type == "accelerator") {
				acceleratorLog.push_back(std::format("vk={} down={}",
					a_msg.value("vk", 0u), a_msg.value("down", false)));
			} else if (type == "webMessage") {
				Print("client: webMessage " + a_msg.value("json", "").substr(0, 160));
			} else if (type == "console") {
				// keep quiet; noisy
			} else if (type == "log") {
				Print(std::format("host[{}]: {}", a_msg.value("level", 0),
					a_msg.value("text", "")));
			} else if (type == "bye") {
				byeSeen = true;
				Print("client: bye (" + a_msg.value("reason", "") + ")");
			}
		}

		void Pump()
		{
			for (;;) {
				json msg;
				{
					std::scoped_lock lock(inboundMutex);
					if (inbound.empty()) break;
					msg = std::move(inbound.front());
					inbound.pop_front();
				}
				HandleInbound(msg);
			}
			if (texturesDirty) {
				texturesDirty = false;
				latestFrame.reset();
				texturesOpened = d3d.OpenRing(pendingTextures, texturesDetail);
				Print("client: " + texturesDetail);
			}
		}

		// Pump + render + Windows messages for a while.
		void RunFor(std::uint32_t a_ms)
		{
			const auto deadline = ::GetTickCount64() + a_ms;
			const auto start = std::chrono::steady_clock::now();
			while (::GetTickCount64() < deadline && running.load()) {
				MSG message{};
				while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
					::TranslateMessage(&message);
					::DispatchMessageW(&message);
				}
				Pump();
				const auto seconds = std::chrono::duration<double>(
					std::chrono::steady_clock::now() - start).count();
				d3d.Render(seconds, overlayVisible, latestFrame);
			}
		}

		bool WaitUntil(std::uint32_t a_timeoutMs, const std::function<bool()>& a_done)
		{
			const auto deadline = ::GetTickCount64() + a_timeoutMs;
			const auto start = std::chrono::steady_clock::now();
			while (::GetTickCount64() < deadline && running.load()) {
				MSG message{};
				while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
					::TranslateMessage(&message);
					::DispatchMessageW(&message);
				}
				Pump();
				if (a_done()) return true;
				const auto seconds = std::chrono::duration<double>(
					std::chrono::steady_clock::now() - start).count();
				d3d.Render(seconds, overlayVisible, latestFrame);
			}
			Pump();
			return a_done();
		}

		// Blocking eval helper (main thread).
		std::optional<std::string> Eval(const std::string& a_script, std::uint32_t a_timeoutMs)
		{
			const auto id = ++lastEvalId;
			Send(json{ { "type", "eval" }, { "id", id }, { "script", a_script } });
			if (!WaitUntil(a_timeoutMs, [&] { return evalResults.contains(id); })) {
				return std::nullopt;
			}
			auto result = std::move(evalResults[id]);
			evalResults.erase(id);
			return result;
		}

		// ----- window -----

		static LRESULT CALLBACK WndProcThunk(HWND a_hwnd, UINT a_msg, WPARAM a_w, LPARAM a_l)
		{
			auto* self = reinterpret_cast<ClientApp*>(
				::GetWindowLongPtrW(a_hwnd, GWLP_USERDATA));
			if (a_msg == WM_NCCREATE) {
				const auto* create = reinterpret_cast<CREATESTRUCTW*>(a_l);
				::SetWindowLongPtrW(a_hwnd, GWLP_USERDATA,
					reinterpret_cast<LONG_PTR>(create->lpCreateParams));
				return ::DefWindowProcW(a_hwnd, a_msg, a_w, a_l);
			}
			return self ? self->WndProc(a_hwnd, a_msg, a_w, a_l) :
				::DefWindowProcW(a_hwnd, a_msg, a_w, a_l);
		}

		LRESULT WndProc(HWND a_hwnd, UINT a_msg, WPARAM a_w, LPARAM a_l)
		{
			switch (a_msg) {
			case WM_SIZE:
				if (a_w != SIZE_MINIMIZED) {
					const auto w = static_cast<std::uint32_t>(LOWORD(a_l));
					const auto h = static_cast<std::uint32_t>(HIWORD(a_l));
					d3d.Resize(w, h);
					if (helloSeen) {
						Send(json{ { "type", "resize" }, { "width", w }, { "height", h } });
					}
				}
				return 0;
			case WM_MOUSEMOVE:
				if (overlayVisible && helloSeen) {
					Send(json{ { "type", "mouse" }, { "kind", "move" },
						{ "x", GET_X_LPARAM(a_l) }, { "y", GET_Y_LPARAM(a_l) } });
				}
				break;
			case WM_LBUTTONDOWN:
			case WM_LBUTTONUP:
				if (overlayVisible && helloSeen) {
					if (a_msg == WM_LBUTTONDOWN) {
						Send(json{ { "type", "focus" }, { "focused", true } });
					}
					Send(json{ { "type", "mouse" }, { "kind", "button" },
						{ "x", GET_X_LPARAM(a_l) }, { "y", GET_Y_LPARAM(a_l) },
						{ "button", 0 }, { "down", a_msg == WM_LBUTTONDOWN } });
					return 0;
				}
				break;
			case WM_MOUSEWHEEL:
				if (overlayVisible && helloSeen) {
					POINT pt{ GET_X_LPARAM(a_l), GET_Y_LPARAM(a_l) };
					::ScreenToClient(a_hwnd, &pt);
					Send(json{ { "type", "mouse" }, { "kind", "wheel" },
						{ "x", pt.x }, { "y", pt.y },
						{ "wheel", GET_WHEEL_DELTA_WPARAM(a_w) } });
					return 0;
				}
				break;
			case WM_KEYDOWN:
				if (a_w == VK_F10 && interactive) {
					SetOverlayVisible(!overlayVisible);
					return 0;
				}
				break;
			case WM_DESTROY:
				running.store(false);
				::PostQuitMessage(0);
				return 0;
			default:
				break;
			}
			return ::DefWindowProcW(a_hwnd, a_msg, a_w, a_l);
		}

		void SetOverlayVisible(bool a_visible)
		{
			overlayVisible = a_visible;
			Send(json{ { "type", "setHidden" }, { "hidden", !a_visible } });
			Send(json{ { "type", "focus" }, { "focused", a_visible } });
			if (!a_visible) {
				::SetFocus(window);  // the game restores its own focus on close
			}
		}

		bool CreateMainWindow()
		{
			WNDCLASSEXW wc{ sizeof(wc) };
			wc.style = CS_HREDRAW | CS_VREDRAW;
			wc.lpfnWndProc = &ClientApp::WndProcThunk;
			wc.hInstance = ::GetModuleHandleW(nullptr);
			wc.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
			wc.lpszClassName = L"OSFUI.WebView2.HostPOC";
			::RegisterClassExW(&wc);
			window = ::CreateWindowExW(0, wc.lpszClassName,
				L"OSF UI WebView2 out-of-process host POC",
				WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
				1280, 760, nullptr, nullptr, wc.hInstance, this);
			return window != nullptr;
		}

		// ----- gates -----

		int RunGates()
		{
			GateResults gates;

			// --- mirror + broker launch -------------------------------------
			const auto hostSource = OwnDirectory() / "osfui_webview2_host.exe";
			mirrorDir = LocalAppData() / "OSFUI" / "bin" / "poc";
			userDataDir = LocalAppData() / "OSFUI" / "WebView2POC";
			std::error_code ec;
			std::filesystem::create_directories(mirrorDir, ec);
			const auto hostMirror = mirrorDir / "osfui_webview2_host.exe";
			std::filesystem::copy_file(hostSource, hostMirror,
				std::filesystem::copy_options::overwrite_existing, ec);
			if (ec) {
				gates.Report("launch", false,
					"host exe mirror copy failed: " + ec.message() +
					" (source " + hostSource.string() + ")");
				return gates.failures;
			}

			std::mt19937_64 rng(::GetTickCount64() ^ (::GetCurrentProcessId() * 2654435761u));
			const auto nonce = static_cast<std::uint32_t>(rng());
			const auto pipeName = std::format(L"{}{}-{:08x}",
				kPipePrefix, ::GetCurrentProcessId(), nonce);
			const auto hostLog = mirrorDir / "host.log";
			const auto args = std::format(L"--pipe={} --game-pid={} --log=\"{}\"",
				pipeName, ::GetCurrentProcessId(), hostLog.native());

			// Server must exist before/while the host retries its connect.
			std::atomic_bool serverUp{ false };
			std::thread serverThread([&] {
				if (pipe.CreateServerAndWait(pipeName, 30000)) {
					serverUp.store(true);
				}
			});
			::Sleep(100);  // let the instance exist before the host launches

			launch = LaunchDetached(hostMirror.native(), args, /*a_preferBroker=*/true);
			Print(std::format("client: launch method={} ok={} detail=[{}]",
				LaunchMethodName(launch.method), launch.ok, launch.detail));
			serverThread.join();
			if (!launch.ok || !serverUp.load()) {
				gates.Report("launch", false, launch.ok ?
					"host never connected to the pipe (see host.log in " +
						mirrorDir.string() + ")" :
					"all launch methods failed: " + launch.detail);
				return gates.failures;
			}
			pipeThread = std::thread([this] { PipeReader(); });

			if (!WaitUntil(10000, [&] { return helloSeen; })) {
				gates.Report("launch", false, "no hello within 10s");
				return gates.failures;
			}
			// Out-of-tree proof: the host's parent must not be this process.
			std::wstring hostParentName = L"<unknown>";
			bool outOfTree = false;
			for (const auto& p : SnapshotProcesses()) {
				if (p.pid == hostPid) {
					outOfTree = p.ppid != ::GetCurrentProcessId();
					hostParentName = ProcessName(p.ppid);
					break;
				}
			}
			gates.Report("launch", launch.ok && outOfTree,
				std::format("method={} host pid={} parent='{}' outOfTree={}",
					LaunchMethodName(launch.method), hostPid,
					ToUtf8(hostParentName), outOfTree));

			// --- init + view -------------------------------------------------
			RECT client{};
			::GetClientRect(window, &client);
			Send(json{
				{ "type", "init" },
				{ "topLevelHwnd", reinterpret_cast<std::uint64_t>(window) },
				{ "viewsPath", ToUtf8(viewsRoot.native()) },
				{ "virtualHost", "osfui.local" },
				{ "width", static_cast<std::uint32_t>(client.right - client.left) },
				{ "height", static_cast<std::uint32_t>(client.bottom - client.top) },
				{ "userDataDir", ToUtf8(userDataDir.native()) },
				{ "devMode", true },
				{ "hidden", false },
			});
			Send(json{ { "type", "accelState" }, { "toggleVk", 0x79 /*F10*/ },
				{ "devReloadVk", 0x7A }, { "captured", true },
				{ "captureArmed", false }, { "captureUpVk", 0 } });
			Send(json{ { "type", "navigate" }, { "id", "osfui/settings" },
				{ "entry", "index.html" }, { "bridge", true } });

			const bool gotFrames = WaitUntil(30000, [&] {
				return texturesOpened && framesSeen >= 3 && domReady;
			});
			// Let the page finish painting real content before sampling pixels
			// (the first captured frames can be a transparent blank document).
			RunFor(1500);
			std::uint64_t opaque = 0;
			if (gotFrames && latestFrame) {
				opaque = d3d.CountOpaquePixels(latestFrame->first, latestFrame->second);
			}
			gates.Report("textures", gotFrames && opaque > 10000,
				std::format("{} — frames={} opaquePixels={} ({} readySeen={})",
					texturesDetail, framesSeen, opaque, gotFrames ? "ok" : "TIMEOUT",
					readySeen));
			if (!gotFrames) {
				Shutdown(gates);
				return gates.failures;
			}

			// --- typing round-trip (cross-process SetParent + real focus) ----
			// The POC may be launched from a background shell: plain
			// SetForegroundWindow is denied then, and SendInput would type into
			// whatever IS foreground. Force it the AttachThreadInput way and
			// VERIFY — in-game this is moot (the game is fullscreen-foreground).
			const auto forceForeground = [&] {
				for (int attempt = 0; attempt < 5; ++attempt) {
					if (::GetForegroundWindow() == window) return true;
					const HWND fg = ::GetForegroundWindow();
					const DWORD fgThread = fg ?
						::GetWindowThreadProcessId(fg, nullptr) : 0;
					const DWORD us = ::GetCurrentThreadId();
					if (fgThread && fgThread != us) {
						::AttachThreadInput(fgThread, us, TRUE);
					}
					::BringWindowToTop(window);
					::SetForegroundWindow(window);
					::SetFocus(window);
					if (fgThread && fgThread != us) {
						::AttachThreadInput(fgThread, us, FALSE);
					}
					RunFor(150);
				}
				return ::GetForegroundWindow() == window;
			};
			const bool foreground = forceForeground();
			if (!foreground) {
				Print("client: WARNING could not take foreground — typing/focus gates unreliable");
			}
			RunFor(250);
			Send(json{ { "type", "focus" }, { "focused", true } });
			RunFor(250);
			const auto prep = Eval(
				"(() => { let i = document.getElementById('osfui_poc_input');"
				" if (!i) { i = document.createElement('input');"
				" i.id = 'osfui_poc_input';"
				" i.style.cssText = 'position:fixed;left:12px;top:12px;width:220px;height:28px;z-index:2147483647';"
				" document.body.appendChild(i); }"
				" i.value = ''; i.focus();"
				" return document.activeElement === i ? 'focused' : 'not-focused'; })()",
				5000);
			// A real click is how focus lands in practice (and how the
			// in-process spike's Gate B acquired it): Chromium pulls OS focus
			// into the right Chrome_WidgetWin_ itself on button-down.
			const auto clickInput = [&] {
				Send(json{ { "type", "mouse" }, { "kind", "move" }, { "x", 60 }, { "y", 24 } });
				RunFor(30);
				Send(json{ { "type", "mouse" }, { "kind", "button" }, { "x", 60 }, { "y", 24 },
					{ "button", 0 }, { "down", true } });
				RunFor(30);
				Send(json{ { "type", "mouse" }, { "kind", "button" }, { "x", 60 }, { "y", 24 },
					{ "button", 0 }, { "down", false } });
				RunFor(150);
			};
			clickInput();
			// Where does the OS think focus is right now? (Implicit input-queue
			// attachment via cross-process SetParent should say Chrome_WidgetWin_.)
			const auto focusClass = [] {
				GUITHREADINFO info{ sizeof(info) };
				wchar_t className[128]{};
				if (::GetGUIThreadInfo(0, &info) && info.hwndFocus) {
					::GetClassNameW(info.hwndFocus, className,
						static_cast<int>(std::size(className)));
				}
				return ToUtf8(className);
			};
			const auto preTypeFocus = focusClass();
			const auto typeUnicode = [&](std::wstring_view a_text) {
				for (const wchar_t ch : a_text) {
					INPUT inputs[2]{};
					inputs[0].type = INPUT_KEYBOARD;
					inputs[0].ki.dwFlags = KEYEVENTF_UNICODE;
					inputs[0].ki.wScan = static_cast<WORD>(ch);
					inputs[1] = inputs[0];
					inputs[1].ki.dwFlags |= KEYEVENTF_KEYUP;
					::SendInput(2, inputs, sizeof(INPUT));
					RunFor(15);
				}
				RunFor(400);
			};
			const auto typeVk = [&](std::string_view a_text) {
				for (const char ch : a_text) {
					const SHORT scan = ::VkKeyScanW(static_cast<WCHAR>(ch));
					if (scan == -1) continue;
					INPUT inputs[2]{};
					inputs[0].type = INPUT_KEYBOARD;
					inputs[0].ki.wVk = LOBYTE(scan);
					inputs[0].ki.wScan = static_cast<WORD>(::MapVirtualKeyW(
						LOBYTE(scan), MAPVK_VK_TO_VSC));
					inputs[1] = inputs[0];
					inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
					::SendInput(2, inputs, sizeof(INPUT));
					RunFor(15);
				}
				RunFor(400);
			};
			typeUnicode(L"GateTyping123");
			auto typed = Eval(
				"(document.getElementById('osfui_poc_input')||{}).value || ''", 5000);
			bool viaUnicode = typed && typed->find("GateTyping123") != std::string::npos;
			bool viaVk = false;
			if (!viaUnicode) {
				// Fallback probe: explicit SetFocus on the Chromium widget from
				// this (attached) thread, then plain VK typing.
				HWND widget = nullptr;
				::EnumChildWindows(window, [](HWND a_hwnd, LPARAM a_param) -> BOOL {
					wchar_t name[128]{};
					::GetClassNameW(a_hwnd, name, static_cast<int>(std::size(name)));
					if (std::wstring_view(name).starts_with(L"Chrome_WidgetWin_")) {
						*reinterpret_cast<HWND*>(a_param) = a_hwnd;
						return FALSE;
					}
					return TRUE;
				}, reinterpret_cast<LPARAM>(&widget));
				if (widget) {
					::SetFocus(widget);
					RunFor(150);
				}
				Eval("(document.getElementById('osfui_poc_input')||{}).value='';"
					 "document.getElementById('osfui_poc_input').focus();'retry'", 3000);
				clickInput();
				typeVk("gatevk77");
				typed = Eval(
					"(document.getElementById('osfui_poc_input')||{}).value || ''", 5000);
				viaVk = typed && typed->find("gatevk77") != std::string::npos;
			}
			gates.Report("typing", viaUnicode || viaVk,
				std::format("prep={} focus='{}' unicode={} vk={} value={}",
					prep.value_or("<eval-timeout>"), preTypeFocus, viaUnicode, viaVk,
					typed.value_or("<eval-timeout>")));

			// --- focus/hide cycles ------------------------------------------
			forceForeground();
			std::uint32_t cyclesPassed = 0;
			for (int cycle = 0; cycle < 20; ++cycle) {
				SetOverlayVisible(false);
				RunFor(60);
				const bool restored = ::GetFocus() == window;
				SetOverlayVisible(true);
				RunFor(90);
				GUITHREADINFO info{ sizeof(info) };
				wchar_t className[128]{};
				bool chromeFocused = false;
				if (::GetGUIThreadInfo(0, &info) && info.hwndFocus) {
					::GetClassNameW(info.hwndFocus, className,
						static_cast<int>(std::size(className)));
					chromeFocused = std::wstring_view(className)
						.starts_with(L"Chrome_WidgetWin_");
				}
				if (restored && chromeFocused) ++cyclesPassed;
			}
			gates.Report("focus", cyclesPassed >= 19,
				std::format("{}/20 hide->restore->show->refocus cycles", cyclesPassed));

			// --- resize ------------------------------------------------------
			const auto framesBefore = framesSeen;
			::SetWindowPos(window, nullptr, 0, 0, 1000, 640,
				SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
			const bool resized = WaitUntil(10000, [&] {
				RECT rc{};
				::GetClientRect(window, &rc);
				return d3d.ringWidth == static_cast<std::uint32_t>(rc.right) &&
					framesSeen > framesBefore && latestFrame &&
					d3d.ringHeight == static_cast<std::uint32_t>(rc.bottom);
			});
			RECT rc{};
			::GetClientRect(window, &rc);
			gates.Report("resize", resized,
				std::format("client {}x{} ring {}x{} frames {} -> {}",
					rc.right, rc.bottom, d3d.ringWidth, d3d.ringHeight,
					framesBefore, framesSeen));

			// --- shutdown ----------------------------------------------------
			if (!interactive) {
				Shutdown(gates);
			}
			return gates.failures;
		}

		void Shutdown(GateResults& a_gates)
		{
			Send(json{ { "type", "shutdown" } });
			// Host teardown destroys windows parented under OUR window: the
			// destroy notifications are cross-process SendMessages to THIS
			// thread, so we must keep pumping while waiting or we deadlock the
			// host (the game's window thread always pumps — this mirrors it).
			bool exited = false;
			if (hostProcess) {
				exited = WaitUntil(7000, [&] {
					return ::WaitForSingleObject(hostProcess, 0) == WAIT_OBJECT_0;
				});
				if (!exited) {
					Print("client: host did not exit in 7s — TerminateProcess");
					::TerminateProcess(hostProcess, 9);
					::WaitForSingleObject(hostProcess, 2000);
				}
			}
			WaitUntil(1500, [&] { return byeSeen; });
			::Sleep(1500);  // give browser children time to unwind
			const auto hostOrphans = DescendantsNamed(hostPid, L"osfui_webview2_host.exe");
			const auto edgeOrphans = DescendantsNamed(hostPid, L"msedgewebview2.exe");
			std::wstring hostName = ProcessName(hostPid);
			const bool hostGone = hostName == L"<gone>" ||
				_wcsicmp(hostName.c_str(), L"osfui_webview2_host.exe") != 0;
			a_gates.Report("shutdown",
				exited && byeSeen && hostGone && edgeOrphans.empty() && hostOrphans.empty(),
				std::format("clean-exit={} bye={} hostGone={} edge-orphans={} host-orphans={}",
					exited, byeSeen, hostGone, edgeOrphans.size(), hostOrphans.size()));
			pipe.Close();
			if (pipeThread.joinable()) pipeThread.join();
		}

		int Run(bool a_interactive)
		{
			interactive = a_interactive;
			viewsRoot = FindViewsRoot();
			if (!std::filesystem::exists(viewsRoot / "osfui" / "settings" / "index.html")) {
				Print("client: views root not found (run from the repo or next to data/)");
				return 10;
			}
			Print("client: views root " + viewsRoot.string());
			if (!CreateMainWindow()) return 11;
			RECT client{};
			::GetClientRect(window, &client);
			if (!d3d.Initialize(window,
					static_cast<std::uint32_t>(client.right - client.left),
					static_cast<std::uint32_t>(client.bottom - client.top))) {
				return 12;
			}
			const int failures = RunGates();
			Print(failures == 0 ? "ALL GATES PASSED" :
				std::format("{} GATE(S) FAILED", failures));
			if (interactive) {
				Print("client: interactive mode — F10 toggles, close the window to exit");
				while (running.load()) {
					RunFor(100);
					if (pipeDead.load()) break;
				}
				GateResults finalGates;
				Shutdown(finalGates);
			}
			return failures == 0 ? 0 : 1;
		}
	};
}

int wmain(int argc, wchar_t** argv)
{
	bool interactive = false;
	for (int i = 1; i < argc; ++i) {
		if (std::wstring_view(argv[i]) == L"--interactive") {
			interactive = true;
		}
	}
	ClientApp app;
	return app.Run(interactive);
}
