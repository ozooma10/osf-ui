#pragma once

#include "composite/ICompositor.h"
#include "core/Config.h"
#include "input/InputRouter.h"
#include "render/IWebRenderer.h"
#include "runtime/MessageBridge.h"
#include "runtime/ViewManager.h"

namespace SWUI
{
	// Owns the whole plugin runtime: config, views, renderer, compositor,
	// bridge, input, and the overlay visibility state. Constructed and
	// initialized from SFSE_PLUGIN_LOAD.
	class Runtime
	{
	public:
		[[nodiscard]] static Runtime& Get();

		bool Initialize();
		void Shutdown();

		// Advances the renderer and submits a frame when visible.
		//
		// Called every frame on the game's Main thread via an SFSE permanent
		// task (core/Plugin.cpp). Runs under SFSE's task-queue lock: keep it
		// cheap, never block. Exact cadence at main menu / pause is still
		// unverified in-game (docs/reverse-engineering-notes.md).
		void Tick(double a_deltaSeconds);

		void SetVisible(bool a_visible);
		void ToggleVisible();
		[[nodiscard]] bool IsVisible() const;

		// Renders and submits one frame if the overlay is visible. Split out
		// from Tick so a future present-side hook can drive submission at a
		// different cadence than logic updates.
		void SubmitFrameIfVisible();

		[[nodiscard]] MessageBridge* Bridge() { return _bridge.get(); }
		[[nodiscard]] const Config&  GetConfig() const { return _config; }

	private:
		Runtime() = default;

		std::unique_ptr<IWebRenderer> CreateRenderer() const;
		std::unique_ptr<ICompositor>  CreateCompositor() const;

		Config                        _config;
		ViewManager                   _views;
		std::unique_ptr<IWebRenderer> _renderer;
		std::unique_ptr<ICompositor>  _compositor;
		std::unique_ptr<MessageBridge> _bridge;
		InputRouter                   _input;
		std::atomic_bool              _visible{ false };
		bool                          _initialized{ false };
	};
}
