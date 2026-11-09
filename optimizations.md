 ### Tier 1: Quick Wins (Eliminate Hidden Redundancy)

  #### 1. Stop Storing the Key 4 Times (KeyTable & ExpiryIndex)

  Currently, when a key has an expiry deadline, the key string is duplicated 4 separate times in memory:

  1. KeyTable::entries_ (std::unordered_map<std::string, Entry>)
  2. KeyTable::keys_ (std::vector<std::string> for random sampling)
  3. ExpiryIndex::entries_ (std::unordered_map<std::string, Entry>)
  4. ExpiryIndex::keys_ (std::vector<std::string> for random sampling)

  • The Problem: In W1/W4/W5, keys are 16 bytes (k000000000000000). Because 16 > 15 (SSO threshold), each key causes 4 independent dynamic heap allocations!
  • The Fix:
      • Keep the owning std::string only once in KeyTable.
      • Let KeyTable::keys_, ExpiryIndex::entries_, and ExpiryIndex::keys_ store std::string_view or const std::string* pointing to the single canonical key.
  • Savings: ~150–200 bytes per expiring key.
  ──────
  #### 2. Move std::mt19937_64 out of SkipList

  Currently, every SkipList embeds its own std::mt19937_64 PRNG:

    sizeof(std::mt19937_64) = 2,504 bytes
  • The Fix: Make the random generator static thread_local std::mt19937_64 or a shared engine instance.
  • Result:
      • sizeof(SkipList) drops from 2,552 B down to ~32 B.
      • sizeof(ZSet) drops from 2,608 B down to ~56 B.
      • At 56 bytes, ZSet can sit inline in the std::variant without needing a pointer indirection (std::unique_ptr), completely eliminating the heap allocation for empty/new sorted sets!
  ──────
  #### 3. Single Allocation for SkipList Nodes (Flexible Array Member)

  In skip_list.h:50-57:

    struct Node {
        double score;
        std::string member;
        std::vector<Level> levels; // <-- 24 bytes struct + SEPARATE heap allocation!
        Node* backward = nullptr;
    };
  Every skip-list node makes two allocations: new Node and new Level[height].

  • The Fix: In C/C++, use a trailing flexible array member or single allocation:
    Node* node = (Node*)::operator new(sizeof(Node) + height * sizeof(Level));
  • Savings: Eliminates 24 bytes of std::vector overhead and 1 heap allocation per member.
  ──────
  ### Tier 2: Eliminating the 15-byte SSO Cliff (Huge for Containers)
  #### 4. Use a 23-Byte SSO String (CompactString)

  Standard std::string in GCC libstdc++ has a 15-byte SSO buffer. In benchmarks, member keys are 16 bytes (m000000000000000), missing SSO by 1 byte and causing:
  • 100 extra heap allocations per List.
  • 50 extra heap allocations per Set.

  A 24-byte string struct (char buffer[23] + uint8_t len) fits all 16-character keys inline:

    struct CompactString {
        // 23 bytes buffer + 1 byte capacity/flag
        // If length <= 23, zero heap allocations!
    };

  • Savings: Eliminates 100% of member string heap allocations in W4 (lists) and W5 (sets), saving ~1,600 to 3,200 bytes per container!
  ──────
  ### Tier 3: Architectural Upgrades (Closing the Gap with Redis)

  #### 5. Replace std::unordered_map with a Flat Open-Addressing Hash Table
  std::unordered_map uses separate-chaining nodes (linked lists):

  • Every entry allocates an 80-byte node (_Hash_node) + 8-byte bucket pointer.
  • In a 1,000,000-key keyspace, this amounts to 1,000,000 pointer nodes and severe memory fragmentation.
  • The Fix: Use a flat open-addressing table (like Google absl::flat_hash_map, ska::flat_hash_map, or a simple Robin Hood hash table):
      • Stores keys and values contiguously in one flat array.
      • No node allocations, no pointers.
  • Savings: ~100–120 bytes per key across the entire database.
  ──────
  #### 6. Compact Encodings for Small Collections (listpack-style)

  (This is how Redis achieves its 326 B hash and 277 B set numbers.)

  Until a Set or Hash grows beyond ~128 elements:

  • Instead of allocating std::unordered_map or std::unordered_set:
  • Store the entries in a flat std::vector<char>:
  [len][field][len][val][len][field][len][val]...
  • Searches scan linearly (for < 128 elements, linear scan in L1 cache is actually faster than hash table pointer chasing).
  • When size > 128, convert ("upgrade") the container to std::unordered_map.
  ──────
  ### Recommended Implementation Roadmap

   Priority                                              │ Feature                                                 │ Effort                                                │ Expected Memory Reduction
  ───────────────────────────────────────────────────────┼─────────────────────────────────────────────────────────┼───────────────────────────────────────────────────────┼───────────────────────────────────────────────────────
   1                                                     │ Share key strings in KeyTable & ExpiryIndex             │ Low (1-2 hours)                                       │ ~15%–20% total database RAM
   2                                                     │ Make mt19937_64 static/thread-local in SkipList         │ Low (15 mins)                                         │ Drops sizeof(ZSet) by 98%
   3                                                     │ Replace levels vector with single-block node allocation │ Medium                                                │ ~30% memory savings in ZSets
   4                                                     │ Flat open-addressing table for KeyTable                 │ Medium                                                │ ~25% savings in all string workloads
   5                                                     │ Compact flat encoding for small Hash/Set                │ High                                                  │ 3x–5x reduction in Hash and Set RAM

