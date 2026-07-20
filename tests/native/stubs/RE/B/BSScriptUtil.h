#pragma once

// Desktop-test stand-in for CommonLibSF's Papyrus glue: just enough of the
// RE::BSScript surface for api/PapyrusApi.cpp to compile UNCHANGED and be
// driven from tests/native/papyrus_action_tests.cpp. Two recording seams:
//   - IVirtualMachine::BindNativeMethod stores each native by name (type-
//     erased); the test fetches it back with its exact function-pointer type
//     and calls it directly — the real marshaling layer is not reproduced.
//   - Internal::VirtualMachine::DispatchStaticCall/DispatchMethodCall resolve
//     the packed args and record the call instead of queueing onto a VM.
// Never used by the real plugin build (the lib/commonlibsf include path wins
// there; this directory is only on the tests' include path).

#include <any>
#include <cassert>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace RE
{
	class BSFixedString
	{
	public:
		BSFixedString() = default;
		BSFixedString(const char* a_str) :
			_str(a_str ? a_str : "") {}
		BSFixedString(const std::string& a_str) :
			_str(a_str) {}

		[[nodiscard]] const char* c_str() const noexcept { return _str.c_str(); }
		[[nodiscard]] bool        empty() const noexcept { return _str.empty(); }

	private:
		std::string _str;
	};

	template <class T>
	class BSTSmartPointer
	{
	public:
		BSTSmartPointer() = default;
		explicit BSTSmartPointer(std::shared_ptr<T> a_ptr) :
			_ptr(std::move(a_ptr)) {}

		[[nodiscard]] T*            get() const noexcept { return _ptr.get(); }
		[[nodiscard]] explicit      operator bool() const noexcept { return _ptr != nullptr; }

	private:
		std::shared_ptr<T> _ptr;
	};

	template <class T>
	using BSScrapArray = std::vector<T>;

	namespace BSScript
	{
		struct Object
		{};

		struct IStackCallbackFunctor
		{};

		// Only string assignment is modeled: the code under test packs
		// BSFixedStrings via its args functor; the test reads them back out of
		// recorded calls.
		class Variable
		{
		public:
			Variable& operator=(const BSFixedString& a_str)
			{
				_str = a_str.c_str();
				return *this;
			}

			[[nodiscard]] const std::string& String() const noexcept { return _str; }

		private:
			std::string _str;
		};

		class IVirtualMachine
		{
		public:
			virtual ~IVirtualMachine() = default;

			template <class F>
			void BindNativeMethod(std::string_view /*a_script*/, std::string_view a_name, F a_func,
				bool /*a_taskletCallable*/ = true, bool /*a_isLatent*/ = false)
			{
				natives[std::string(a_name)] = a_func;
			}

			// Test-side accessor: F must be the native's exact function-pointer
			// type (any_cast is exact-match).
			template <class F>
			[[nodiscard]] F GetNative(std::string_view a_name) const
			{
				const auto it = natives.find(std::string(a_name));
				assert(it != natives.end());
				return std::any_cast<F>(it->second);
			}

			std::map<std::string, std::any> natives;
		};

		namespace Internal
		{
			class VirtualMachine : public IVirtualMachine
			{
			public:
				struct Call
				{
					bool                     isStatic{ false };
					std::string              scriptName;            // static calls
					const Object*            receiver{ nullptr };   // method calls
					std::string              fn;
					std::vector<std::string> args;
				};

				static VirtualMachine* GetSingleton()
				{
					static VirtualMachine instance;
					return &instance;
				}

				template <class Fn>
				bool DispatchStaticCall(const BSFixedString& a_script, const BSFixedString& a_fn, Fn&& a_makeArgs,
					const BSTSmartPointer<IStackCallbackFunctor>&, int)
				{
					calls.push_back({ true, a_script.c_str(), nullptr, a_fn.c_str(), Resolve(std::forward<Fn>(a_makeArgs)) });
					return true;
				}

				template <class Fn>
				bool DispatchMethodCall(const BSTSmartPointer<Object>& a_receiver, const BSFixedString& a_fn, Fn&& a_makeArgs,
					const BSTSmartPointer<IStackCallbackFunctor>&, int)
				{
					calls.push_back({ false, "", a_receiver.get(), a_fn.c_str(), Resolve(std::forward<Fn>(a_makeArgs)) });
					return true;
				}

				std::vector<Call> calls;

			private:
				template <class Fn>
				static std::vector<std::string> Resolve(Fn&& a_makeArgs)
				{
					BSScrapArray<Variable> packed;
					a_makeArgs(packed);
					std::vector<std::string> out;
					out.reserve(packed.size());
					for (const auto& v : packed) {
						out.push_back(v.String());
					}
					return out;
				}
			};
		}
	}

	class GameVM
	{
	public:
		static GameVM* GetSingleton()
		{
			static GameVM instance;
			return &instance;
		}

		[[nodiscard]] BSScript::Internal::VirtualMachine* GetVM()
		{
			return BSScript::Internal::VirtualMachine::GetSingleton();
		}
	};
}
