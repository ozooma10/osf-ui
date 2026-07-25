#pragma once

struct ID3D12Device;
struct ID3D12Resource;

namespace OSFUI::WorldTextureProbe
{
	// Enables the investigation-only SRV observer. TryInstall remains a no-op
	// unless this was called, so normal configurations never touch the device
	// vtable.
	void Enable();
	[[nodiscard]] bool IsEnabled();

	// Supplies the opened WebView ring texture used by the one-screen proof.
	// The probe owns a COM reference until replaced or cleared.
	void SetReplacementTexture(ID3D12Resource* a_resource);


	// Installs a dev-only hook on ID3D12Device::CreateShaderResourceView. Calls
	// are forwarded unchanged except for the uniquely sized diagnostic cockpit
	// texture, whose descriptor can sample the supplied WebView ring resource.
	bool TryInstall(ID3D12Device* a_device);
}
