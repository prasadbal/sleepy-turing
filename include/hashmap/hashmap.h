#pragma once
#include <tsl/robin_map.h>
#include <tsl/robin_set.h>

namespace marketlib {

template<typename K, typename V,
         typename Hash  = std::hash<K>,
         typename KeyEq = std::equal_to<K>>
using HashMap = tsl::robin_map<K, V, Hash, KeyEq>;

template<typename K,
         typename Hash  = std::hash<K>,
         typename KeyEq = std::equal_to<K>>
using HashSet = tsl::robin_set<K, Hash, KeyEq>;

} // namespace marketlib
