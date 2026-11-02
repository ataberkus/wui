#include <doctest/doctest.h>
#include "../../src/layout/flow_detail.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <wui/layout.hpp>

TEST_CASE("declared rows resolve before children and flow never overlaps") {
    const std::array columns{wui::Column{wui::ColumnKind::fixed, 100},
                             wui::Column{wui::ColumnKind::share, 1},
                             wui::Column{wui::ColumnKind::share, 2}};
    const auto cells = std::get<std::vector<wui::Rect>>(
        wui::row_cells({0, 0, 416, 100}, columns, 8));
    REQUIRE(cells.size() == 3);
    CHECK(cells[0].x == 0);
    CHECK(cells[0].y == 0);
    CHECK(cells[0].w == 100);
    CHECK(cells[0].h == 100);
    CHECK(cells[1].x == doctest::Approx(108));
    CHECK(cells[1].w == doctest::Approx(100));
    CHECK(cells[2].x == doctest::Approx(216));
    CHECK(cells[2].w == doctest::Approx(200));

    wui::Flow flow({10, 20, 300, 500}, 8);
    const auto a = std::get<wui::Rect>(flow.take(36));
    const auto b = std::get<wui::Rect>(flow.take(20));
    CHECK(a.x == 10);
    CHECK(a.y == 20);
    CHECK(a.w == 300);
    CHECK(a.h == 36);
    CHECK(b.y == a.y + a.h + 8);
    CHECK(flow.used_height() == doctest::Approx(64));
    CHECK(std::holds_alternative<wui::Error>(flow.take(-1)));
}

TEST_CASE("row validation handles empty rows and rejects unusable declarations") {
    const std::array<wui::Column, 0> empty{};
    const auto empty_cells = wui::row_cells({5, 6, 7, 8}, empty, 2);
    REQUIRE(std::holds_alternative<std::vector<wui::Rect>>(empty_cells));
    CHECK(std::get<std::vector<wui::Rect>>(empty_cells).empty());

    const std::array zero_weight{wui::Column{wui::ColumnKind::share, 0}};
    CHECK(std::holds_alternative<wui::Error>(
        wui::row_cells({0, 0, 100, 20}, zero_weight, 0)));

    const std::array too_wide{wui::Column{wui::ColumnKind::fixed, 60},
                              wui::Column{wui::ColumnKind::fixed, 50}};
    CHECK(std::holds_alternative<wui::Error>(
        wui::row_cells({0, 0, 100, 20}, too_wide, 0)));
}

TEST_CASE("layout rejects nonfinite values and flow can grow beyond container height") {
    const auto infinity = std::numeric_limits<float>::infinity();
    const std::array fixed{wui::Column{wui::ColumnKind::fixed, 10}};
    CHECK(std::holds_alternative<wui::Error>(
        wui::row_cells({0, 0, 100, 20}, fixed, infinity)));

    wui::Flow invalid({0, 0, 100, 20}, -1);
    CHECK(std::holds_alternative<wui::Error>(invalid.take(10)));

    wui::Flow growing({3, 4, 50, 10}, 2);
    const auto first = std::get<wui::Rect>(growing.take(12));
    const auto second = std::get<wui::Rect>(growing.take(5));
    CHECK(first.h == 12);
    CHECK(second.y == 18);
    CHECK(growing.used_height() == 19);
}

TEST_CASE("row rejects geometry whose resolved edge exceeds float range") {
    const auto maximum = std::numeric_limits<float>::max();
    const std::array fixed{wui::Column{wui::ColumnKind::fixed, maximum}};

    CHECK(std::holds_alternative<wui::Error>(
        wui::row_cells({maximum, 0, maximum, 20}, fixed, 0)));
}

TEST_CASE("row append preserves previous cells on late geometry and allocation failure") {
    class FailingResource final : public std::pmr::memory_resource {
    public:
        bool fail = false;
    private:
        void* do_allocate(std::size_t bytes, std::size_t alignment) override {
            if (fail) throw std::bad_alloc{};
            return std::pmr::new_delete_resource()->allocate(bytes, alignment);
        }
        void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
            std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
        }
        bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
            return this == &other;
        }
    } resource;
    std::pmr::vector<wui::Rect> cells{&resource};
    cells.push_back({1, 2, 3, 4});
    const auto maximum = std::numeric_limits<float>::max();
    const std::array overflow{wui::Column{wui::ColumnKind::fixed, 0},
                              wui::Column{wui::ColumnKind::share, 1}};
    const auto invalid = wui::detail::append_row_cells(
        {maximum, 0, maximum, 20}, overflow, 0, cells);
    REQUIRE(std::holds_alternative<wui::Error>(invalid));
    CHECK(std::get<wui::Error>(invalid).code == wui::ErrorCode::invalid_argument);
    REQUIRE(cells.size() == 1);
    CHECK(cells[0].x == 1);
    CHECK(cells[0].w == 3);

    std::pmr::vector<wui::Rect> growth{&resource};
    growth.push_back({1, 2, 3, 4});
    resource.fail = true;
    const std::array fixed{wui::Column{wui::ColumnKind::fixed, 20}};
    CHECK_THROWS_AS(wui::detail::append_row_cells({0, 0, 40, 20}, fixed, 0, growth),
                    std::bad_alloc);
    REQUIRE(growth.size() == 1);
    CHECK(growth[0].x == 1);
    CHECK(growth[0].w == 3);
}

TEST_CASE("visible rows return a bounded half-open range and reject invalid extents") {
    auto range = wui::visible_rows(10'000, 20.0f, 2'020.0f, 100.0f);
    REQUIRE(std::holds_alternative<wui::VisibleRows>(range));
    CHECK(std::get<wui::VisibleRows>(range).first == 101);
    CHECK(std::get<wui::VisibleRows>(range).last == 106);
    for (float height : {0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN()}) {
        auto result = wui::visible_rows(10, height, 0, 100);
        REQUIRE(std::holds_alternative<wui::Error>(result));
        CHECK(std::get<wui::Error>(result).code == wui::ErrorCode::invalid_argument);
    }
    auto negative = wui::visible_rows(10, 20, -1, 100);
    REQUIRE(std::holds_alternative<wui::Error>(negative));
    CHECK(std::get<wui::Error>(negative).code == wui::ErrorCode::invalid_argument);
    auto overflow = wui::visible_rows(2, std::numeric_limits<float>::max(), 0, 100);
    REQUIRE(std::holds_alternative<wui::Error>(overflow));
    CHECK(std::get<wui::Error>(overflow).code == wui::ErrorCode::capacity);
    for (float bad : {-1.0f, std::numeric_limits<float>::quiet_NaN()}) {
        auto result = wui::visible_rows(10, 20, 0, bad);
        REQUIRE(std::holds_alternative<wui::Error>(result));
        CHECK(std::get<wui::Error>(result).code == wui::ErrorCode::invalid_argument);
    }
}

TEST_CASE("refused flow rows report the precise error and leave the flow unchanged") {
    const auto infinity = std::numeric_limits<float>::infinity();
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    const auto message = [](wui::Result<wui::Rect> result) {
        REQUIRE(std::holds_alternative<wui::Error>(result));
        return std::get<wui::Error>(result).message;
    };
    const std::string invalid = "flow dimensions and gap must be finite and nonnegative";
    const std::string overflow = "flow dimensions overflow";

    wui::Flow flow({0, 3e38f, 10, 10}, 4);
    CHECK(message(flow.take(-1)) == invalid);
    CHECK(message(flow.take(nan)) == invalid);
    CHECK(message(flow.take(infinity)) == invalid);
    CHECK(message(flow.take(3e38f)) == overflow); // y + height exceeds float range
    CHECK(flow.used_height() == 0);
    wui::Rect row{};
    CHECK_FALSE(wui::detail::FlowAccess::take(flow, 3e38f, row));
    CHECK_FALSE(wui::detail::FlowAccess::take(flow, infinity, row));
    CHECK_FALSE(wui::detail::FlowAccess::take(flow, nan, row));

    // Refusals did not advance the flow: the first accepted row has no gap.
    REQUIRE(wui::detail::FlowAccess::take(flow, 2, row));
    CHECK(row.y == 3e38f);
    CHECK(row.h == 2);
    const auto next = std::get<wui::Rect>(flow.take(1));
    CHECK(next.y == 3e38f + 6.0f);
    CHECK(flow.used_height() == 7);

    wui::Flow broken({0, 0, 10, 10}, nan);
    CHECK(message(broken.take(1)) == invalid);
    CHECK_FALSE(wui::detail::FlowAccess::take(broken, 1, row));
}

namespace {
// The original field-by-field layout checks and row resolution; the library
// must agree with them exactly.
bool reference_valid_area(wui::Rect area, float gap) {
    return std::isfinite(area.x) && std::isfinite(area.y) && std::isfinite(area.w) &&
           std::isfinite(area.h) && area.w >= 0.0f && area.h >= 0.0f &&
           std::isfinite(gap) && gap >= 0.0f;
}
bool reference_representable(double value) {
    return std::isfinite(value) && value >= std::numeric_limits<float>::lowest() &&
           value <= std::numeric_limits<float>::max();
}
std::optional<std::vector<wui::Rect>> reference_cells(
    wui::Rect area, std::span<const wui::Column> columns, float gap) {
    if (!reference_valid_area(area, gap)) return std::nullopt;
    double fixed_sum = 0.0, weight_sum = 0.0;
    for (const auto column : columns) {
        if (!std::isfinite(column.amount)) return std::nullopt;
        if (column.kind == wui::ColumnKind::fixed) {
            if (column.amount < 0.0f) return std::nullopt;
            fixed_sum += column.amount;
        } else {
            if (column.amount <= 0.0f) return std::nullopt;
            weight_sum += column.amount;
        }
    }
    std::vector<wui::Rect> cells;
    if (columns.empty()) return cells;
    const double gap_sum = static_cast<double>(gap) * (columns.size() - 1);
    const double remaining = static_cast<double>(area.w) - fixed_sum - gap_sum;
    if (!std::isfinite(fixed_sum) || !std::isfinite(weight_sum) || !std::isfinite(gap_sum) ||
        remaining < 0.0)
        return std::nullopt;
    if (!reference_representable(static_cast<double>(area.y) + area.h)) return std::nullopt;
    double x = area.x;
    for (const auto column : columns) {
        const double width = column.kind == wui::ColumnKind::fixed
                                 ? static_cast<double>(column.amount)
                                 : remaining * column.amount / weight_sum;
        const double right = x + width;
        if (!reference_representable(width) || !reference_representable(x) ||
            !reference_representable(right))
            return std::nullopt;
        cells.push_back({static_cast<float>(x), area.y, static_cast<float>(width), area.h});
        x = right + gap;
    }
    return cells;
}
bool same_bits(float a, float b) {
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}
}

TEST_CASE("flow and row checks match their field-by-field definitions exactly") {
    constexpr float maximum = std::numeric_limits<float>::max();
    const float values[] = {0.0f, -0.0f, 1.0f, -1.0f, 2.5f, 1e-40f, -1e-40f, 3e38f, -3e38f,
                            maximum, -maximum, std::numeric_limits<float>::infinity(),
                            -std::numeric_limits<float>::infinity(),
                            std::numeric_limits<float>::quiet_NaN()};
    // A flow accepts a zero-height row exactly when its area and gap are valid.
    for (const float a : values) {
        for (const float b : values) {
            for (const wui::Rect area : {wui::Rect{a, 0, b, 10}, wui::Rect{0, a, 10, b},
                                         wui::Rect{a, b, 1, 1}, wui::Rect{1, 1, a, b}}) {
                for (const float gap : {0.0f, 4.0f, a, b}) {
                    wui::Flow flow(area, gap);
                    wui::Rect row{};
                    CHECK(wui::detail::FlowAccess::take(flow, 0, row) ==
                          reference_valid_area(area, gap));
                }
            }
        }
    }
    // Rows near and beyond the float range resolve, or fail, exactly as before.
    for (const float x : values) {
        for (const float amount : values) {
            for (const float w : {100.0f, maximum, 3e38f, -1.0f}) {
                const std::array<wui::Column, 3> columns{{{wui::ColumnKind::fixed, amount},
                                                          {wui::ColumnKind::share, 1},
                                                          {wui::ColumnKind::share, 2}}};
                for (const wui::Rect area : {wui::Rect{x, 0, w, 20}, wui::Rect{0, x, w, 20},
                                             wui::Rect{x, 3e38f, w, 3e38f}}) {
                    for (const float gap : {0.0f, 8.0f}) {
                        const auto want = reference_cells(area, columns, gap);
                        const auto got = wui::row_cells(area, columns, gap);
                        REQUIRE(std::holds_alternative<std::vector<wui::Rect>>(got) ==
                                want.has_value());
                        if (!want) continue;
                        const auto& cells = std::get<std::vector<wui::Rect>>(got);
                        REQUIRE(cells.size() == want->size());
                        for (std::size_t i = 0; i < cells.size(); ++i) {
                            CHECK((same_bits(cells[i].x, (*want)[i].x) &&
                                   same_bits(cells[i].y, (*want)[i].y) &&
                                   same_bits(cells[i].w, (*want)[i].w) &&
                                   same_bits(cells[i].h, (*want)[i].h)));
                        }
                    }
                }
            }
        }
    }
}
