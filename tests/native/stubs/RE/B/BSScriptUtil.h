#pragma once


#include <any>
#include <cassert>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace RE
{
	class TESForm;

	class BSFixedString
	{
	public:
		BSFixedString() = default;
		BSFixedString(const char* a_str) :
			m_str(a_str ? a_str : "") {}
		BSFixedString(const std::string& a_str) :
			m_str(a_str) {}

		[[nodiscard]] const char* c_str() const noexcept { return m_str.c_str(); }
		[[nodiscard]] bool        empty() const noexcept { return m_str.empty(); }

	private:
		std::string m_str;
	};

	template <class T>
	class BSTSmartPointer
	{
	public:
		BSTSmartPointer() = default;
		explicit BSTSmartPointer(std::shared_ptr<T> a_ptr) :
			m_ptr(std::move(a_ptr)) {}

		[[nodiscard]] T*            get() const noexcept { return m_ptr.get(); }
		[[nodiscard]] T*            operator->() const noexcept { return m_ptr.get(); }
		[[nodiscard]] explicit      operator bool() const noexcept { return m_ptr != nullptr; }

	private:
		std::shared_ptr<T> m_ptr;
	};

	template <class T>
	using BSScrapArray = std::vector<T>;

	namespace BSScript
	{
		struct ObjectTypeInfo
		{
			BSFixedString                    name;
			BSTSmartPointer<ObjectTypeInfo> parentTypeInfo;
		};

		struct Object
		{
			BSTSmartPointer<ObjectTypeInfo> type;
			TESForm*                        form{ nullptr };
			std::size_t                     handle{ 0 };
			bool                            valid{ true };

			[[nodiscard]] bool        IsValid() const noexcept { return valid; }
			[[nodiscard]] std::size_t GetHandle() { return handle; }
		};

		struct IObjectHandlePolicy
		{
			virtual ~IObjectHandlePolicy() = default;
			virtual bool        IsHandleObjectAvailable(std::size_t a_handle) const { return !unavailable.contains(a_handle); }
			virtual std::size_t EmptyHandle() const { return 0; }

			std::set<std::size_t> unavailable;
		};

		struct IStackCallbackFunctor
		{};

		class Variable
		{
		public:
			Variable& operator=(std::nullptr_t)
			{
				m_str.clear();
				m_type = "none";
				m_isList = false;
				m_object = {};
				m_inner.reset();
				return *this;
			}
			Variable& operator=(const BSFixedString& a_str)
			{
				m_str = a_str.c_str();
				m_type = "string";
				m_isList = false;
				return *this;
			}
			Variable& operator=(std::int32_t a_value)
			{
				m_str = std::to_string(a_value);
				m_type = "int";
				m_isList = false;
				return *this;
			}
			Variable& operator=(float a_value)
			{
				m_str = std::to_string(a_value);
				m_type = "float";
				m_isList = false;
				return *this;
			}
			Variable& operator=(bool a_value)
			{
				m_str = a_value ? "true" : "false";
				m_type = "bool";
				m_isList = false;
				return *this;
			}
			Variable& operator=(BSTSmartPointer<Object> a_object)
			{
				m_object = std::move(a_object);
				m_type = "object";
				m_isList = false;
				return *this;
			}
			Variable& operator=(Variable* a_value)
			{
				m_inner.reset(a_value);
				m_type = "var";
				m_isList = false;
				return *this;
			}

			template <class T>
			[[nodiscard]] bool is() const noexcept
			{
				if constexpr (std::same_as<T, std::nullptr_t>) return m_type == "none";
				if constexpr (std::same_as<T, BSFixedString>) return m_type == "string";
				if constexpr (std::same_as<T, std::int32_t> || std::same_as<T, std::uint32_t>) return m_type == "int";
				if constexpr (std::same_as<T, float>) return m_type == "float";
				if constexpr (std::same_as<T, bool>) return m_type == "bool";
				if constexpr (std::same_as<T, Object>) return m_type == "object";
				if constexpr (std::same_as<T, Variable>) return m_type == "var";
				return false;
			}

			[[nodiscard]] const std::string& String() const noexcept { return m_str; }
			[[nodiscard]] const std::string& Type() const noexcept { return m_type; }

			[[nodiscard]] bool                             IsList() const noexcept { return m_isList; }
			[[nodiscard]] const std::vector<std::string>&  List() const noexcept { return m_list; }
			[[nodiscard]] const std::vector<std::string>&  ListTypes() const noexcept { return m_listTypes; }
			[[nodiscard]] const BSTSmartPointer<Object>& ObjectValue() const noexcept { return m_object; }
			[[nodiscard]] const Variable* InnerValue() const noexcept { return m_inner.get(); }
			void SetList(std::vector<std::string> a_list, std::vector<std::string> a_types = {})
			{
				m_list = std::move(a_list);
				m_listTypes = std::move(a_types);
				m_type = "array";
				m_isList = true;
			}

		private:
			std::string              m_str;
			std::string              m_type{ "none" };
			std::vector<std::string> m_list;
			std::vector<std::string> m_listTypes;
			BSTSmartPointer<Object>  m_object;
			std::shared_ptr<Variable> m_inner;
			bool                     m_isList{ false };
		};

		inline void PackVariable(Variable& a_var, const std::vector<BSFixedString>& a_values)
		{
			std::vector<std::string> out;
			out.reserve(a_values.size());
			for (const auto& v : a_values) {
				out.emplace_back(v.c_str());
			}
			a_var.SetList(std::move(out));
		}

		inline void PackVariable(Variable& a_var, const std::vector<const Variable*>& a_values)
		{
			std::vector<std::string> out;
			std::vector<std::string> types;
			out.reserve(a_values.size());
			types.reserve(a_values.size());
			for (const auto* value : a_values) {
				out.push_back(value ? value->String() : "");
				types.push_back(value ? value->Type() : "none");
				delete value;
			}
			a_var.SetList(std::move(out), std::move(types));
		}

		inline void PackVariable(Variable& a_var, const BSFixedString& a_value) { a_var = a_value; }
		inline void PackVariable(Variable& a_var, const std::string& a_value) { a_var = BSFixedString(a_value); }
		inline void PackVariable(Variable& a_var, std::int32_t a_value) { a_var = a_value; }
		inline void PackVariable(Variable& a_var, float a_value) { a_var = a_value; }
		inline void PackVariable(Variable& a_var, bool a_value) { a_var = a_value; }
		inline void PackVariable(Variable& a_var, TESForm* a_value)
		{
			if (!a_value) {
				a_var = nullptr;
				return;
			}
			auto type = std::make_shared<ObjectTypeInfo>();
			type->name = BSFixedString("Form");
			auto object = std::make_shared<Object>();
			object->type = BSTSmartPointer<ObjectTypeInfo>(std::move(type));
			object->form = a_value;
			a_var = BSTSmartPointer<Object>(std::move(object));
		}

		template <class T>
		[[nodiscard]] auto get(const Variable& a_var)
		{
			if constexpr (std::same_as<T, BSFixedString>) return BSFixedString(a_var.String());
			if constexpr (std::same_as<T, std::int32_t>) return static_cast<std::int32_t>(std::stoi(a_var.String()));
			if constexpr (std::same_as<T, std::uint32_t>) return static_cast<std::uint32_t>(std::stoul(a_var.String()));
			if constexpr (std::same_as<T, float>) return std::stof(a_var.String());
			if constexpr (std::same_as<T, bool>) return a_var.String() == "true";
			if constexpr (std::same_as<T, Object>) return a_var.ObjectValue();
			if constexpr (std::same_as<T, Variable>) return const_cast<Variable*>(a_var.InnerValue());
		}

		template <class T>
		[[nodiscard]] T* UnpackVariable(const Variable& a_var)
			requires(std::same_as<T, TESForm>)
		{
			return a_var.ObjectValue() ? a_var.ObjectValue()->form : nullptr;
		}

		class IVirtualMachine
		{
		public:
			virtual ~IVirtualMachine() = default;

			template <class F>
			void BindNativeMethod(std::string_view a_script, std::string_view a_name, F a_func,
				bool /*a_taskletCallable*/ = true, bool /*a_isLatent*/ = false)
			{
				natives[std::string(a_name)] = a_func;
				nativeScripts[std::string(a_name)].push_back(std::string(a_script));
			}

			template <class F>
			[[nodiscard]] F GetNative(std::string_view a_name) const
			{
				const auto it = natives.find(std::string(a_name));
				assert(it != natives.end());
				return std::any_cast<F>(it->second);
			}

			[[nodiscard]] const IObjectHandlePolicy& GetObjectHandlePolicy() const { return handlePolicy; }
			[[nodiscard]] IObjectHandlePolicy&       GetObjectHandlePolicy() { return handlePolicy; }

			// Scripts named in unknownScripts fail to load; everything else resolves to a fresh type.
			bool GetScriptObjectType(const BSFixedString& a_name, BSTSmartPointer<ObjectTypeInfo>& a_type)
			{
				if (unknownScripts.contains(a_name.c_str())) return false;
				auto type = std::make_shared<ObjectTypeInfo>();
				type->name = a_name;
				a_type = BSTSmartPointer<ObjectTypeInfo>(std::move(type));
				return true;
			}

			// Objects registered in boundObjects are what the VM binds to a handle.
			bool FindBoundObject(std::uint64_t a_handle, const char*, bool, BSTSmartPointer<Object>& a_object, bool) const
			{
				const auto it = boundObjects.find(a_handle);
				if (it == boundObjects.end()) return false;
				a_object = it->second;
				return true;
			}

			std::map<std::string, std::any> natives;
			std::map<std::string, std::vector<std::string>> nativeScripts;
			IObjectHandlePolicy handlePolicy;
			std::set<std::string> unknownScripts;
			std::map<std::uint64_t, BSTSmartPointer<Object>> boundObjects;
		};

		namespace Internal
		{
			class VirtualMachine : public IVirtualMachine
			{
			public:
				struct Call
				{
					bool                     isStatic{ false };
					std::string              scriptName;            // static and handle-addressed method calls
					const Object*            receiver{ nullptr };   // object-addressed method calls
					std::uint64_t            handle{ 0 };           // handle-addressed method calls
					std::string              fn;
					std::vector<std::string> args;
					std::vector<std::string> argTypes;
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
					if (!staticDispatchSucceeds) {
						return false;
					}
					auto packed = Resolve(std::forward<Fn>(a_makeArgs));
					calls.push_back({ true, a_script.c_str(), nullptr, 0, a_fn.c_str(), std::move(packed.args), std::move(packed.types) });
					return true;
				}

				template <class Fn>
				bool DispatchMethodCall(const BSTSmartPointer<Object>& a_receiver, const BSFixedString& a_fn, Fn&& a_makeArgs,
					const BSTSmartPointer<IStackCallbackFunctor>&, int)
				{
					auto packed = Resolve(std::forward<Fn>(a_makeArgs));
					calls.push_back({ false, "", a_receiver.get(), 0, a_fn.c_str(), std::move(packed.args), std::move(packed.types) });
					return true;
				}

				template <class Fn>
				bool DispatchMethodCall(std::uint64_t a_handle, const BSFixedString& a_script, const BSFixedString& a_fn, Fn&& a_makeArgs,
					const BSTSmartPointer<IStackCallbackFunctor>&, int)
				{
					if (!boundObjects.contains(a_handle)) {
						return false;
					}
					auto packed = Resolve(std::forward<Fn>(a_makeArgs));
					calls.push_back({ false, a_script.c_str(), nullptr, a_handle, a_fn.c_str(), std::move(packed.args), std::move(packed.types) });
					return true;
				}

				std::vector<Call> calls;
				bool              staticDispatchSucceeds{ true };

			private:
				struct Packed
				{
					std::vector<std::string> args;
					std::vector<std::string> types;
				};

				template <class Fn>
				static Packed Resolve(Fn&& a_makeArgs)
				{
					BSScrapArray<Variable> packed;
					a_makeArgs(packed);
					Packed out;
					out.args.reserve(packed.size());
					out.types.reserve(packed.size());
					for (const auto& v : packed) {
						if (v.IsList()) {
							for (std::size_t i = 0; i < v.List().size(); ++i) {
								out.args.push_back(v.List()[i]);
								out.types.push_back(i < v.ListTypes().size() ? v.ListTypes()[i] : "string");
							}
						} else {
							out.args.push_back(v.String());
							out.types.push_back(v.Type());
						}
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
