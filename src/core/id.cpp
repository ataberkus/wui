#include <wui/id.hpp>

#include "id_hash.hpp"

namespace wui {

Id child_id(Id parent, std::string_view key) { return detail::hash_child_id(parent, key); }

} // namespace wui
