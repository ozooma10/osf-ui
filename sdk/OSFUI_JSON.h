// ============================================================================
// OSFUI_JSON.h - optional nlohmann::json authoring conveniences for OSF UI.
//
// Include this beside OSFUI_API.h when your plugin already uses nlohmann/json.
// It is header-only, allocates nothing across the DLL boundary, and leaves the
// dependency-free OSFUI ABI unchanged. Link nothing.
// ============================================================================
#pragma once

#include "OSFUI_API.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace OSFUI::API
{
	using Json = nlohmann::json;

	// Parsed callback payload shared by JsonCommand and JsonRequest. Incoming
	// plugin payloads are required to be JSON objects; invalid input is exposed
	// without throwing from construction. Require()/As() retain nlohmann's
	// normal typed-conversion exceptions, while Value()/TryGet() are no-throw
	// conveniences for optional fields.
	class JsonPayload
	{
	public:
		explicit JsonPayload(const char* a_payloadJson) noexcept
		{
			_payload = Json::parse(a_payloadJson ? a_payloadJson : "{}", nullptr, false);
			if (_payload.is_discarded()) {
				_error = "payload is not valid JSON";
				_payload = Json::object();
			} else if (!_payload.is_object()) {
				_error = "payload must be a JSON object";
				_payload = Json::object();
			}
		}

		[[nodiscard]] explicit operator bool() const noexcept { return _error.empty(); }
		[[nodiscard]] bool IsValid() const noexcept { return _error.empty(); }
		[[nodiscard]] std::string_view Error() const noexcept { return _error; }
		[[nodiscard]] const Json& Payload() const noexcept { return _payload; }

		template <class T>
		[[nodiscard]] T Require(std::string_view a_key) const
		{
			return _payload.at(std::string(a_key)).template get<T>();
		}

		template <class T>
		[[nodiscard]] T Value(std::string_view a_key, T a_fallback) const noexcept
		{
			try {
				const auto it = _payload.find(std::string(a_key));
				return it == _payload.end() ? std::move(a_fallback) : it->template get<T>();
			} catch (...) {
				return std::move(a_fallback);
			}
		}

		template <class T>
		[[nodiscard]] bool TryGet(std::string_view a_key, T& a_out) const noexcept
		{
			try {
				const auto it = _payload.find(std::string(a_key));
				if (it == _payload.end()) return false;
				a_out = it->template get<T>();
				return true;
			} catch (...) {
				return false;
			}
		}

		template <class T>
		[[nodiscard]] T As() const
		{
			return _payload.template get<T>();
		}

	protected:
		Json        _payload{ Json::object() };
		std::string _error;
	};

	// Drop-in parser for RegisterCommand callbacks.
	class JsonCommand final : public JsonPayload
	{
	public:
		JsonCommand(const char* a_command, const char* a_payloadJson, const char* a_sourceViewId) noexcept :
			JsonPayload(a_payloadJson),
			_command(a_command ? a_command : ""),
			_sourceViewId(a_sourceViewId ? a_sourceViewId : "")
		{}

		[[nodiscard]] std::string_view Command() const noexcept { return _command; }
		[[nodiscard]] std::string_view SourceViewId() const noexcept { return _sourceViewId; }

	private:
		std::string_view _command;
		std::string_view _sourceViewId;
	};

	// Drop-in parser/responder for RegisterRequest callbacks. Invalid JSON is
	// rejected immediately with code "invalid-payload", so a forgotten validity
	// check cannot strand the page until its timeout.
	class JsonRequest final : public JsonPayload
	{
	public:
		explicit JsonRequest(const Request& a_request) noexcept :
			JsonPayload(a_request.payloadJson),
			_request(a_request)
		{
			if (!IsValid()) {
				_request.Reject("invalid-payload", _error.c_str());
			}
		}

		[[nodiscard]] std::string_view Command() const noexcept
		{
			return _request.command ? _request.command : "";
		}
		[[nodiscard]] std::string_view SourceViewId() const noexcept
		{
			return _request.sourceViewId ? _request.sourceViewId : "";
		}
		[[nodiscard]] const Request& Raw() const noexcept { return _request; }

		// Read a required field without throwing. Missing or wrongly typed
		// fields reject the request immediately as invalid-payload.
		template <class T>
		[[nodiscard]] std::optional<T> Get(std::string_view a_key) const noexcept
		{
			try {
				const auto it = _payload.find(std::string(a_key));
				if (it != _payload.end()) return it->template get<T>();
			} catch (...) {}

			try {
				const auto message = "missing or invalid field: " + std::string(a_key);
				_request.Reject("invalid-payload", message.c_str());
			} catch (...) {
				_request.Reject("invalid-payload", "missing or invalid required field");
			}
			return std::nullopt;
		}

		bool Respond(const Json& a_payload) const noexcept { return RespondImpl(nullptr, a_payload); }
		bool Respond(const char* a_type, const Json& a_payload) const noexcept { return RespondImpl(a_type, a_payload); }

		template <class T>
		bool Respond(const T& a_value) const noexcept
		{
			try {
				return Respond(Json(a_value));
			} catch (...) {
				_request.Reject("serialization-error", "could not serialize response");
				return false;
			}
		}

		template <class T>
		bool Respond(const char* a_type, const T& a_value) const noexcept
		{
			try {
				return Respond(a_type, Json(a_value));
			} catch (...) {
				_request.Reject("serialization-error", "could not serialize response");
				return false;
			}
		}

		void Reject(const char* a_code, const char* a_message = "") const noexcept
		{
			_request.Reject(a_code, a_message);
		}

	private:
		bool RespondImpl(const char* a_type, const Json& a_payload) const noexcept
		{
			if (!IsValid()) return false;
			try {
				const auto text = a_payload.dump();
				if (a_type && *a_type) _request.Respond(a_type, text.c_str());
				else _request.Respond(text.c_str());
				return true;
			} catch (...) {
				_request.Reject("serialization-error", "could not serialize response");
				return false;
			}
		}

		Request _request;
	};

	// JSON overloads for every Client method that otherwise requires authors to
	// call dump() and manage a temporary string manually.
	class JsonClient
	{
	public:
		explicit JsonClient(const Client& a_client) noexcept : _client(a_client) {}

		[[nodiscard]] bool SendToWeb(const char* a_viewId, const char* a_type, const Json& a_payload) const noexcept
		{
			try {
				const auto text = a_payload.dump();
				return _client.SendToWeb(a_viewId, a_type, text.c_str());
			} catch (...) {
				return false;
			}
		}

		template <class T>
		[[nodiscard]] bool SendToWeb(const char* a_viewId, const char* a_type, const T& a_payload) const noexcept
		{
			try {
				return SendToWeb(a_viewId, a_type, Json(a_payload));
			} catch (...) {
				return false;
			}
		}

		// Retained state (ABI 1.8). The C ABI takes JSON TEXT — a const char* is
		// the only shape that survives the vtable contract — so the ergonomic
		// overloads live here, exactly like the Respond ladder above, and
		// nlohmann never crosses the DLL boundary.
		//
		// Reach for this instead of SendToWeb whenever the value is true until
		// it changes: OSF UI replays state to every document that loads, so a
		// view fed this way survives F5 with no re-push handshake on either
		// side. SendToWeb stays for things that HAPPENED.
		[[nodiscard]] bool SetViewState(const char* a_modId, const char* a_key, const Json& a_value) const noexcept
		{
			try {
				const auto text = a_value.dump();
				return _client.SetViewState(a_modId, a_key, text.c_str());
			} catch (...) {
				return false;
			}
		}

		template <class T>
		[[nodiscard]] bool SetViewState(const char* a_modId, const char* a_key, const T& a_value) const noexcept
		{
			try {
				return SetViewState(a_modId, a_key, Json(a_value));
			} catch (...) {
				return false;
			}
		}

		[[nodiscard]] bool RegisterSettingsSchema(const Json& a_schema) const noexcept
		{
			try {
				const auto text = a_schema.dump();
				return _client.RegisterSettingsSchema(text.c_str());
			} catch (...) {
				return false;
			}
		}

		[[nodiscard]] bool ReportIssue(const char* a_modId, const char* a_id, const char* a_code,
			IssueSeverity a_severity, const char* a_subject = "", const Json& a_context = Json::object()) const noexcept
		{
			try {
				const auto text = a_context.dump();
				return _client.ReportIssue(a_modId, a_id, a_code, a_severity, a_subject, text.c_str());
			} catch (...) {
				return false;
			}
		}

		[[nodiscard]] bool ClearIssuesExcept(const char* a_modId, const std::vector<std::string>& a_keepIds) const noexcept
		{
			try {
				const auto text = Json(a_keepIds).dump();
				return _client.ClearIssuesExcept(a_modId, text.c_str());
			} catch (...) {
				return false;
			}
		}

	private:
		const Client& _client;
	};
}
