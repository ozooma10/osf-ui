        void Compose()
        {
            if (!output.Pending()) return;
            std::vector<View*> visible;
            bool complete = true, anyVisible = false;
            for (auto& [id, view] : views) if (!view->hidden && !view->closing) {
                anyVisible = true;
                if (!view->loaded || !view->reveal.Ready() || !view->surface.texture) {
                    complete = false;
                    continue;
                }
                visible.push_back(view.get());
            }
            if (!anyVisible) { output.NoViews(); return; }
            // A requested view can still be waiting for its reveal paint. Do
            // not publish an empty partial composition as its first frame.
            // Keep dirty work pending; the next paint will schedule it. Ready
            // siblings may still publish while another view loads or fails.
            if (visible.empty()) return;
            const auto now = FramePacer::Clock::now();
            const bool fresh = std::any_of(visible.begin(), visible.end(), [](auto* view) {
                return view->paintSerial != view->publishedPaintSerial;
            });
            const bool waiting = visible.size() > 1 && std::any_of(visible.begin(), visible.end(), [now](auto* view) {
                return view->paintSerial == view->publishedPaintSerial &&
                    now - view->lastPaint < 2 * FramePacer::period;
            });
            if (fresh && waiting && ScheduleFrame(true)) return;
            std::stable_sort(visible.begin(), visible.end(), [](auto* a, auto* b) { return a->order < b->order; });
            const float x = (static_cast<float>(width) - viewportWidth) * 0.5f;
            const float y = (static_cast<float>(height) - viewportHeight) * 0.5f;
            std::vector<Layer> layers;
            for (auto* view : visible) {
                const auto bounds=ViewBounds(*view);
                const float left=x+bounds.x, top=y+bounds.y;
                layers.push_back({ &view->surface, left, top, static_cast<float>(bounds.width), static_cast<float>(bounds.height) });
                if (view->popupShown && view->popup.texture) {
                    const float scale = static_cast<float>(view->Scale());
                    layers.push_back({ &view->popup, left + view->popupRect.x * scale, top + view->popupRect.y * scale,
                        view->popupRect.width * scale, view->popupRect.height * scale });
                }
            }
            if (graphics.Publish(layers, width, height, epoch, complete)) {
                for (auto* view : visible) view->publishedPaintSerial = view->paintSerial;
                output.Published(FramePacer::Clock::now());
            } else output.Blocked(); // New paints replace surfaces; an ack wakes publication.
        }
