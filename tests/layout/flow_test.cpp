#include <doctest/doctest.h>
#include "../../src/layout/flow_detail.hpp"

#include <array>
#include <cmath>
#include <limits>
#include <memory_resource>
#include <new>
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
