#include "painter.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace wui::detail {

void emit_text(FrameState& frame, std::string_view value, FontRole role,
               TextOptions options, bool description, bool validation) {
    if (!frame.check_canvas()) return;
    if (!std::isfinite(options.size) || options.size < 0) {
        frame.report(ErrorCode::invalid_argument, "Text size must be finite and nonnegative", true);
        return;
    }
    const float size = options.size;
    const Color color = validation ? invalid_ink(frame.theme) :
        options.color.value_or(description ? frame.theme.secondary : frame.theme.text);
    for (float channel : {color.r, color.g, color.b, color.a}) {
        if (!std::isfinite(channel) || channel < 0 || channel > 1) {
            frame.report(ErrorCode::invalid_argument, "Text color must be finite and normalized", true);
            return;
        }
    }
    const Id id = frame.text_id();
    if (!frame.register_widget(id) || !frame.copy_label(value)) return;

    if (!options.wrap) {
        const auto run = frame.shape(value, role, size);
        if (!run) return;
        const Rect row = frame.take(run->height);
        if (frame.error || !frame.check_canvas()) return;
        {
            WidgetClip clip(frame, intersect(row, frame.clip()));
            if (!frame.check_canvas()) return;
            frame.glyphs(*run, {row.x + align_offset(options.align, row.w, run->width), row.y}, color);
            if (frame.check_canvas())
                frame.canvas.semantic(id, validation ? Role::validation : Role::text,
                                      value, row, !validation, false, false, {}, validation);
        }
        frame.check_canvas();
        return;
    }

    frame.wrap_lines.clear();
    // Shape each explicit line first: sanitized display and grapheme starts come
    // from the text service, so a width break never bisects a combining cluster.
    auto flow = frame.stack.back().flow;
    const auto allocation = flow.take(0);
    if (const auto* error = std::get_if<Error>(&allocation)) {
        frame.report(error->code, error->message.c_str(), true);
        return;
    }
    const float width = std::get<Rect>(allocation).w;
    for (std::size_t start = 0; start <= value.size();) {
        const auto newline = value.find('\n', start);
        const auto segment = value.substr(start, newline == std::string_view::npos
                                                      ? newline : newline - start);
        const auto run = frame.shape(segment, role, size);
        if (!run) return;
        const std::string_view display = run->display;
        if (display.empty()) {
            frame.wrap_lines.push_back(run);
        } else {
            const auto& starts = run->grapheme_starts;
            if (starts.empty() || starts.front() != 0) {
                frame.report(ErrorCode::invalid_font, "Text service omitted grapheme boundaries");
                return;
            }
            for (std::size_t from = 0; from < display.size();) {
                const std::size_t first = std::lower_bound(starts.begin(), starts.end(), from) - starts.begin();
                const std::size_t count = starts.size() - first;
                std::size_t low = 1, high = count + 1, fit = 0;
                std::shared_ptr<const TextRun> best;
                while (low < high) {
                    const std::size_t mid = low + (high - low) / 2;
                    const std::size_t end = first + mid < starts.size()
                                                ? starts[first + mid] : display.size();
                    const auto candidate = frame.shape(display.substr(from, end - from), role, size);
                    if (!candidate) return;
                    if (candidate->width <= width) {
                        fit = mid;
                        best = candidate;
                        low = mid + 1;
                    } else {
                        high = mid;
                    }
                }
                if (!fit) {
                    fit = 1; // Even a clipped first grapheme must advance.
                    const auto end = first + 1 < starts.size()
                                         ? starts[first + 1] : display.size();
                    best = frame.shape(display.substr(from, end - from), role, size);
                    if (!best) return;
                }
                std::size_t end = first + fit < starts.size()
                                      ? starts[first + fit] : display.size();
                if (end < display.size()) {
                    const auto space = display.rfind(' ', end - 1);
                    if (space != std::string_view::npos && space > from) {
                        end = space;
                        best = frame.shape(display.substr(from, end - from), role, size);
                        if (!best) return;
                    }
                }
                frame.wrap_lines.push_back(std::move(best));
                from = end;
                while (from < display.size() && display[from] == ' ') ++from;
            }
        }
        if (newline == std::string_view::npos) break;
        start = newline + 1;
    }
    double height = 0;
    for (const auto& line : frame.wrap_lines) height += line->height;
    if (!std::isfinite(height) || height > std::numeric_limits<float>::max()) {
        frame.report(ErrorCode::capacity, "Wrapped text exceeds layout limits");
        return;
    }
    const Rect row = frame.take(static_cast<float>(height));
    if (frame.error || !frame.check_canvas()) return;
    {
        WidgetClip clip(frame, intersect(row, frame.clip()));
        if (!frame.check_canvas()) return;
        float y = row.y;
        for (const auto& line : frame.wrap_lines) {
            frame.glyphs(*line, {row.x + align_offset(options.align, row.w, line->width), y}, color);
            if (!frame.check_canvas()) return;
            y += line->height;
        }
        frame.canvas.semantic(id, validation ? Role::validation : Role::text,
                              value, row, !validation, false, false, {}, validation);
    }
    frame.check_canvas();
}

} // namespace wui::detail
