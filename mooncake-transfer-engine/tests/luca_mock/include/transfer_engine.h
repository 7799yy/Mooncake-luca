#pragma once
#include "common.h"
#include "transport/transport.h"
namespace mooncake {
// Use the same friendship as the real engine, without linking its GPU/network
// dependencies. Calls still dispatch to the production transport's overrides.
class TransferEngine {
   public:
    static int registerBuffer(Transport& t, void* p, size_t n) {
        return t.registerLocalMemory(p, n, "cuda:0", true, true);
    }
    static int registerBatch(
        Transport& t, const std::vector<Transport::BufferEntry>& buffers) {
        return t.registerLocalMemoryBatch(buffers, "cuda:0");
    }
    static int unregisterBuffer(Transport& t, void* p) {
        return t.unregisterLocalMemory(p, true);
    }
    static int unregisterBatch(Transport& t, const std::vector<void*>& p) {
        return t.unregisterLocalMemoryBatch(p);
    }
};
}  // namespace mooncake
