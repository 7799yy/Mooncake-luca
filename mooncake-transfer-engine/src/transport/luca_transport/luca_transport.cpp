// Copyright 2024 KVCache.AI
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Adapted from Mooncake MacaTransport at
// 3aa0f716008c0cac25ffc82ef234d6b0a59e9c2e. Cross v2/range checks retain
// the experimentally verified LUCA protocol from this branch's parent.
#include "transport/luca_transport/luca_transport.h"
#include <cuda.h>
#include <lcr/lc_runtime_api.h>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <utility>
#include <vector>
#include "config.h"

namespace mooncake {
namespace {
bool nativeOK(lcError_t rc, const char* operation) {
    if (rc == lcSuccess) return true;
    LOG(ERROR) << "LucaTransport: " << operation << " rc=" << int(rc);
    return false;
}
bool allocationRange(void* ptr, size_t length, uint64_t& base, size_t& size) {
    CUdeviceptr allocation = 0;
    CUresult rc = cuMemGetAddressRange(&allocation, &size,
                                       reinterpret_cast<CUdeviceptr>(ptr));
    if (rc != CUDA_SUCCESS) {
        // Never guess the allocation base: an IPC import is relocated from it.
        LOG(ERROR) << "LucaTransport: cuMemGetAddressRange rc=" << int(rc)
                   << " ptr=" << ptr;
        return false;
    }
    base = static_cast<uint64_t>(allocation);
    const uint64_t address = reinterpret_cast<uint64_t>(ptr);
    if (!base || !size || address < base || address - base > size ||
        length > size - (address - base)) {
        LOG(ERROR) << "LucaTransport: range outside allocation";
        return false;
    }
    return true;
}
std::string toHex(const void* ptr, size_t size) {
    static const char digits[] = "0123456789abcdef";
    const auto* bytes = static_cast<const unsigned char*>(ptr);
    std::string text(size * 2, '0');
    for (size_t i = 0; i < size; ++i) {
        text[2 * i] = digits[bytes[i] >> 4];
        text[2 * i + 1] = digits[bytes[i] & 15];
    }
    return text;
}
bool fromHex(const std::string& text, void* ptr, size_t size) {
    if (text.size() != size * 2) return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    auto* bytes = static_cast<unsigned char*>(ptr);
    for (size_t i = 0; i < size; ++i) {
        int high = nibble(text[2 * i]), low = nibble(text[2 * i + 1]);
        if (high < 0 || low < 0) return false;
        bytes[i] = static_cast<unsigned char>((high << 4) | low);
    }
    return true;
}
}  // namespace

namespace {
// MACA's default/auto/batchflag selection, using native LUCA types throughout.
enum class CopyApi { Auto, Default, BatchFlag };
struct DeviceScope {
    int original = -1;
    bool valid;
    DeviceScope()
        : valid(nativeOK(lcGetDevice(&original), "lcGetDevice") &&
                nativeOK(lcSetDevice(0), "lcSetDevice")) {}
    ~DeviceScope() {
        if (original >= 0) nativeOK(lcSetDevice(original), "restore device");
    }
};
void copyEndpoints(Transport::Slice* slice, void*& dst, void*& src) {
    dst = slice->local.dest_addr;
    src = slice->source_addr;
    if (slice->opcode == Transport::TransferRequest::READ) std::swap(dst, src);
}
}  // namespace

struct LucaTransport::Impl {
    explicit Impl(LucaTransport& transport) : owner(transport) {}
    struct Mapping {
        void* address = nullptr;
        bool retired = false;
        // Closed explicitly only after the last in-flight user completes.
    };
    struct ExportedAllocation {
        lcIpcCrossMemHandle_t handle{};
        size_t size = 0;
        size_t users = 0;
    };
    struct Command {
        Slice* single = nullptr;
        std::vector<lcCopyFlag_t> batch;
    };
    struct Pending {
        std::vector<Slice*> slices;
        std::vector<std::shared_ptr<Mapping>> mappings;
        // Keep SDK batch descriptors alive until completion, not just enqueue.
        std::vector<Command> commands;
        lcEvent_t done{};
        int64_t started = 0;
        bool failed = false;
        bool needs_drain = false;
        bool completed = false;
        bool drain_error_logged = false;
        bool bad_event = false;
    };
    using MappingKey = std::tuple<SegmentID, uint64_t, std::string>;
    LucaTransport& owner;
    std::mutex mutex;
    std::string host_id;
    CopyApi copy_api = CopyApi::Auto;
    size_t batch_min_bytes = 1024ULL * 1024ULL;
    lcStream_t stream{};
    std::vector<lcEvent_t> events;
    std::map<void*, BufferDesc> registered;
    std::map<void*, uint64_t> registered_base;
    std::map<uint64_t, ExportedAllocation> exported;
    std::map<MappingKey, std::weak_ptr<Mapping>> mappings;
    std::map<TransferTask*, std::shared_ptr<Pending>> pending;
    std::vector<std::shared_ptr<Mapping>> retired_mappings;

    bool configure() {
        if (const char* value = std::getenv("MC_LUCA_COPY_API")) {
            std::string name(value);
            if (name == "auto")
                copy_api = CopyApi::Auto;
            else if (name == "default")
                copy_api = CopyApi::Default;
            else if (name == "batchflag")
                copy_api = CopyApi::BatchFlag;
            else {
                LOG(ERROR) << "Invalid MC_LUCA_COPY_API";
                return false;
            }
        }
        if (const char* value = std::getenv("MC_LUCA_BATCHFLAG_MIN_BYTES")) {
            if (!std::isdigit(static_cast<unsigned char>(*value))) return false;
            char* end = nullptr;
            errno = 0;
            unsigned long long bytes = std::strtoull(value, &end, 0);
            if (!*value || *value == '-' || end == value || *end ||
                errno == ERANGE || bytes > std::numeric_limits<size_t>::max())
                return false;
            batch_min_bytes = static_cast<size_t>(bytes);
        }
        return true;
    }
    bool useBatch(size_t length) const {
        return copy_api == CopyApi::BatchFlag ||
               (copy_api == CopyApi::Auto && length >= batch_min_bytes);
    }
    bool ensureStream() {
        return stream ||
               nativeOK(lcStreamCreateWithFlags(&stream, lcStreamNonBlocking),
                        "lcStreamCreateWithFlags");
    }
    bool acquireEvent(lcEvent_t& event) {
        if (!events.empty()) {
            event = events.back();
            events.pop_back();
            return true;
        }
        return nativeOK(lcEventCreateWithFlags(&event, lcEventDisableTiming),
                        "lcEventCreateWithFlags");
    }
    void closeMappings(Pending& context) {
        for (auto& mapping : context.mappings) {
            if (mapping.use_count() != 1 || !mapping->address) continue;
            if (nativeOK(lcIpcCloseMemHandle(mapping->address),
                         "lcIpcCloseMemHandle")) {
                mapping->address = nullptr;
            } else {
                // GPU work is complete, but retain a failed close for teardown.
                context.failed = true;
                mapping->retired = true;
                retired_mappings.push_back(mapping);
            }
        }
        context.mappings.clear();
    }
    void finish(const std::shared_ptr<Pending>& context) {
        if (context->completed) return;
        // Caller has established event completion or a successful device drain.
        closeMappings(*context);
        if (context->done) {
            if (context->bad_event)
                nativeOK(lcEventDestroy(context->done), "failed event destroy");
            else
                events.push_back(context->done);
            context->done = {};
        }
        for (auto* slice : context->slices) {
            if (context->failed)
                slice->markFailed();
            else
                slice->markSuccess();
            slice->task->is_finished = true;
            pending.erase(slice->task);
        }
        context->slices.clear();
        context->commands.clear();
        context->completed = true;
    }
    void progress(const std::shared_ptr<Pending>& context, bool drain = false) {
        if (context->completed) return;
        if (globalConfig().slice_timeout > 0 &&
            getCurrentTimeInNano() - context->started >
                int64_t(globalConfig().slice_timeout) * 1000000000LL) {
            context->failed = true;
            context->needs_drain = true;
        }
        if (drain || context->needs_drain) {
            lcError_t rc = lcDeviceSynchronize();
            if (rc != lcSuccess) {
                context->failed = true;
                context->needs_drain = true;
                if (!context->drain_error_logged) {
                    nativeOK(rc, "in-flight drain; resources retained");
                    context->drain_error_logged = true;
                }
                return;  // Not terminal: freeBatchID must continue to refuse.
            }
            finish(context);
            return;
        }
        lcError_t rc = lcEventQuery(context->done);
        if (rc == lcSuccess)
            finish(context);
        else if (rc != lcErrorNotReady) {
            nativeOK(rc, "lcEventQuery");
            context->bad_event = true;
            context->failed = true;
            context->needs_drain = true;
            progress(context, true);
        }
    }
    bool drainAll() {
        std::vector<std::shared_ptr<Pending>> contexts;
        for (const auto& entry : pending) {
            if (std::find(contexts.begin(), contexts.end(), entry.second) ==
                contexts.end())
                contexts.push_back(entry.second);
        }
        for (const auto& context : contexts) progress(context, true);
        return pending.empty();
    }
    void shutdown() {
        std::lock_guard<std::mutex> lock(mutex);
        if (pending.empty() && !stream && events.empty() &&
            retired_mappings.empty())
            return;
        DeviceScope device;
        if (!device.valid || !drainAll()) {
            // A failed GPU drain is not cancellation. Preserve descriptors and
            // imports until process exit rather than close memory still in use.
            for (const auto& entry : pending)
                (void)new std::shared_ptr<Pending>(entry.second);
            LOG(ERROR) << "LucaTransport destroyed with undrained work; native "
                          "resources quarantined";
            return;
        }
        for (const auto& mapping : retired_mappings)
            if (mapping->address)
                nativeOK(lcIpcCloseMemHandle(mapping->address),
                         "retired mapping close");
        for (auto event : events)
            nativeOK(lcEventDestroy(event), "lcEventDestroy");
        if (stream) nativeOK(lcStreamDestroy(stream), "lcStreamDestroy");
    }

    bool resolve(const TransferRequest& request, Pending& context,
                 void*& peer) {
        if (request.target_id == LOCAL_SEGMENT_ID) {
            peer = reinterpret_cast<void*>(request.target_offset);
            uint64_t base = 0;
            size_t size = 0;
            return allocationRange(peer, request.length, base, size);
        }
        auto segment = owner.metadata_->getSegmentDescByID(request.target_id);
        if (!segment || segment->protocol != "luca") return false;
        for (const auto& buffer : segment->buffers) {
            const uint64_t address = request.target_offset;
            if (address < buffer.addr ||
                address - buffer.addr > buffer.length ||
                request.length > buffer.length - (address - buffer.addr))
                continue;
            Json::Value envelope;
            Json::Reader reader;
            if (!reader.parse(buffer.shm_name, envelope, false) ||
                !envelope.isObject() || !envelope["format"].isString() ||
                envelope["format"].asString() != "luca-cross-v2" ||
                !envelope["abi_size"].isUInt64() ||
                envelope["abi_size"].asUInt64() !=
                    sizeof(lcIpcCrossMemHandle_t) ||
                !envelope["host_id"].isString() ||
                envelope["host_id"].asString() != host_id ||
                !envelope["allocation_base"].isUInt64() ||
                !envelope["allocation_size"].isUInt64() ||
                !envelope["handle_hex"].isString())
                return false;
            uint64_t base = envelope["allocation_base"].asUInt64();
            uint64_t size = envelope["allocation_size"].asUInt64();
            if (!base || !size || buffer.addr < base ||
                buffer.addr - base > size ||
                buffer.length > size - (buffer.addr - base) || address < base ||
                address - base > size ||
                request.length > size - (address - base))
                return false;
            const auto hex = envelope["handle_hex"].asString();
            lcIpcCrossMemHandle_t handle{};
            if (!fromHex(hex, &handle, sizeof(handle))) return false;
            MappingKey key{request.target_id, base,
                           toHex(&handle, sizeof(handle))};
            auto mapping = mappings[key].lock();
            if (!mapping || mapping->retired) {
                mapping = std::make_shared<Mapping>();
                if (!nativeOK(lcIpcOpenMemHandleCross_v2(
                                  &mapping->address, &handle,
                                  lcIpcMemLazyEnablePeerAccess),
                              "lcIpcOpenMemHandleCross_v2"))
                    return false;
                // Hold even an invalid/null mapping until the common cleanup
                // path.
                context.mappings.push_back(mapping);
                mappings[key] = mapping;
            } else if (std::find(context.mappings.begin(),
                                 context.mappings.end(),
                                 mapping) == context.mappings.end()) {
                context.mappings.push_back(mapping);
            }
            const uint64_t mapped =
                reinterpret_cast<uint64_t>(mapping->address);
            const uint64_t offset = address - base;
            if (!mapped ||
                offset > std::numeric_limits<uint64_t>::max() - mapped)
                return false;
            peer = reinterpret_cast<void*>(mapped + offset);
            return true;
        }
        return false;
    }

    Status submit(const std::vector<TransferTask*>& tasks) {
        for (auto* task : tasks)
            if (!task || !task->request || task->slice_count ||
                !task->slice_list.empty())
                return Status::InvalidArgument(
                    "LucaTransport: invalid or reused task");
        if (tasks.empty()) return Status::OK();
        auto context = std::make_shared<Pending>();
        context->started = getCurrentTimeInNano();
        context->slices.reserve(tasks.size());
        context->mappings.reserve(tasks.size());
        for (auto it = mappings.begin(); it != mappings.end();)
            if (it->second.expired())
                it = mappings.erase(it);
            else
                ++it;
        DeviceScope device;
        for (auto* task : tasks) {
            const auto& request = *task->request;
            task->total_bytes = request.length;
            task->transport_ = &owner;
            task->is_finished = false;
            Slice* slice = owner.getSliceCache().allocate();
            slice->source_addr = request.source;
            slice->local.dest_addr = nullptr;
            slice->length = request.length;
            slice->opcode = request.opcode;
            slice->target_id = request.target_id;
            slice->task = task;
            slice->status = Slice::PENDING;
            slice->ts = 0;  // This backend drains timed-out GPU work itself.
            task->slice_list.push_back(slice);
            __sync_fetch_and_add(&task->slice_count, 1);
            bool opcode = request.opcode == TransferRequest::READ ||
                          request.opcode == TransferRequest::WRITE;
            if (opcode && !request.length) {
                slice->markSuccess();
                task->is_finished = true;
                continue;
            }
            uint64_t base = 0;
            size_t size = 0;
            if (!device.valid || !opcode || !request.source ||
                !allocationRange(request.source, request.length, base, size) ||
                !resolve(request, *context, slice->local.dest_addr)) {
                slice->markFailed();
                task->is_finished = true;
                continue;
            }
            context->slices.push_back(slice);
        }
        if (context->slices.empty()) {
            closeMappings(*context);
            return Status::OK();
        }
        // Prepare all descriptor storage before enqueue. Contiguous runs retain
        // request ordering when auto mode mixes memcpy and batchflag commands.
        for (auto* slice : context->slices) {
            if (!useBatch(slice->length)) {
                context->commands.emplace_back();
                context->commands.back().single = slice;
            } else {
                if (context->commands.empty() ||
                    context->commands.back().single)
                    context->commands.emplace_back();
                lcCopyFlag_t copy{};
                void* dst;
                void* src;
                copyEndpoints(slice, dst, src);
                copy.dst = dst;
                copy.src = src;
                copy.count = slice->length;
                copy.engine = ParallelCopyEngineDefault;
                copy.waitNum = 0;
                copy.writeNum = 0;
                context->commands.back().batch.push_back(copy);
            }
        }
        for (auto* slice : context->slices) pending[slice->task] = context;
        // The vLLM sender can be an executor thread. Keep one device-wide
        // producer barrier per submit until its actual producer event is wired.
        if (!nativeOK(lcDeviceSynchronize(), "batch producer-ready barrier") ||
            !ensureStream() || !acquireEvent(context->done)) {
            context->failed = true;
            finish(context);  // Nothing from this context was enqueued.
            return Status::OK();
        }
        context->needs_drain = true;
        for (auto& command : context->commands) {
            lcError_t rc;
            if (command.single) {
                void* dst;
                void* src;
                copyEndpoints(command.single, dst, src);
                rc = lcMemcpyAsync(dst, src, command.single->length,
                                   lcMemcpyDefault, stream);
            } else {
                // SDK takes a mutable descriptor pointer; storage is retained
                // by this context until the recorded event has completed.
                auto& batch = command.batch;
                rc = lcExtBatchCopyFlagAndWaitV2(batch.data(), batch.size(),
                                                 nullptr, 0, stream);
            }
            if (!nativeOK(rc, "async copy enqueue")) {
                context->failed = true;
                progress(context, true);
                return Status::OK();
            }
        }
        if (!nativeOK(lcEventRecord(context->done, stream), "lcEventRecord")) {
            context->bad_event = true;
            context->failed = true;
            progress(context, true);
            return Status::OK();
        }
        context->needs_drain = false;
        for (auto* slice : context->slices) slice->status = Slice::POSTED;
        return Status::OK();
    }

    int registerMemory(void* ptr, size_t length, const std::string& location) {
        if (!ptr || !length || registered.count(ptr)) return -1;
        uint64_t base = 0;
        size_t size = 0;
        if (!allocationRange(ptr, length, base, size)) return -1;
        auto allocation = exported.find(base);
        if (allocation == exported.end()) {
            ExportedAllocation value;
            value.size = size;
            if (!nativeOK(lcIpcGetMemHandleCross_v2(
                              &value.handle, reinterpret_cast<void*>(base)),
                          "lcIpcGetMemHandleCross_v2"))
                return -1;
            allocation = exported.emplace(base, value).first;
        } else if (allocation->second.size != size)
            return -1;
        const auto& handle = allocation->second.handle;
        Json::Value envelope;
        envelope["format"] = "luca-cross-v2";
        envelope["abi_size"] = static_cast<Json::UInt64>(sizeof(handle));
        envelope["host_id"] = host_id;
        envelope["allocation_base"] = static_cast<Json::UInt64>(base);
        envelope["allocation_size"] = static_cast<Json::UInt64>(size);
        envelope["handle_hex"] = toHex(&handle, sizeof(handle));
        BufferDesc buffer{};
        buffer.name = location.empty() ? "cuda:0" : location;
        buffer.addr = reinterpret_cast<uint64_t>(ptr);
        buffer.length = length;
        buffer.shm_name = envelope.toStyledString();
        int rc = owner.metadata_->addLocalMemoryBuffer(buffer, false);
        if (!rc) {
            registered.emplace(ptr, buffer);
            registered_base.emplace(ptr, base);
            ++allocation->second.users;
        } else if (!allocation->second.users)
            exported.erase(allocation);
        return rc;
    }
    void forgetRegistration(void* ptr) {
        auto allocation = exported.find(registered_base.at(ptr));
        if (--allocation->second.users == 0) exported.erase(allocation);
        registered_base.erase(ptr);
        registered.erase(ptr);
    }
    int registerBatch(const std::vector<BufferEntry>& entries,
                      const std::string& location, bool update) {
        std::vector<void*> inserted;
        for (const auto& entry : entries) {
            int rc = registerMemory(entry.addr, entry.length, location);
            if (rc) {
                for (auto* ptr : inserted) {
                    owner.metadata_->removeLocalMemoryBuffer(ptr, false);
                    forgetRegistration(ptr);
                }
                return rc;
            }
            inserted.push_back(entry.addr);
        }
        int rc = update ? owner.metadata_->updateLocalSegmentDesc() : 0;
        if (rc)
            for (auto* ptr : inserted) {
                owner.metadata_->removeLocalMemoryBuffer(ptr, false);
                forgetRegistration(ptr);
            }
        return rc;
    }
    int unregisterBatch(const std::vector<void*>& entries, bool update) {
        std::set<void*> unique;
        for (auto* ptr : entries)
            if (!registered.count(ptr) || !unique.insert(ptr).second) return -1;
        if (!drainAll()) return -1;
        std::vector<BufferDesc> removed;
        int rc = 0;
        for (auto* ptr : entries) {
            rc = owner.metadata_->removeLocalMemoryBuffer(ptr, false);
            if (rc) break;
            removed.push_back(registered.at(ptr));
        }
        if (!rc && update) rc = owner.metadata_->updateLocalSegmentDesc();
        if (rc) {
            for (const auto& buffer : removed)
                owner.metadata_->addLocalMemoryBuffer(buffer, false);
        } else {
            for (auto* ptr : entries) forgetRegistration(ptr);
        }
        return rc;
    }
};

LucaTransport::LucaTransport() : impl_(new Impl(*this)) {}
LucaTransport::~LucaTransport() { impl_->shutdown(); }

int LucaTransport::install(std::string& name,
                           std::shared_ptr<TransferMetadata> meta,
                           std::shared_ptr<Topology>) {
    int count = 0;
    if (!impl_->configure() ||
        !nativeOK(lcGetDeviceCount(&count), "lcGetDeviceCount") || count != 1) {
        LOG(ERROR)
            << "LucaTransport requires one visible GPU and valid copy options";
        return -1;
    }
    DeviceScope device;
    if (!device.valid) return -1;
    const char* value = std::getenv("MC_LUCA_HOST_ID");
    if (value && *value)
        impl_->host_id = value;
    else {
        std::ifstream file("/proc/sys/kernel/random/boot_id");
        std::getline(file, impl_->host_id);
    }
    if (impl_->host_id.empty()) return -1;
    metadata_ = std::move(meta);
    local_server_name_ = name;
    auto desc = std::make_shared<SegmentDesc>();
    desc->name = name;
    desc->protocol = "luca";
    LOG(INFO) << "LucaTransport: MACA-derived async/batchflag, Cross v2, one "
                 "visible GPU";
    return metadata_->addLocalSegment(LOCAL_SEGMENT_ID, name, std::move(desc));
}
Status LucaTransport::submitTransferTask(
    const std::vector<TransferTask*>& tasks) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->submit(tasks);
}
Status LucaTransport::submitTransfer(
    BatchID id, const std::vector<TransferRequest>& requests) {
    auto& batch = *reinterpret_cast<BatchDesc*>(id);
    if (batch.task_list.size() > batch.batch_size ||
        requests.size() > batch.batch_size - batch.task_list.size())
        return Status::TooManyRequests(
            "LucaTransport: batch capacity exceeded");
    size_t start = batch.task_list.size();
    batch.task_list.resize(start + requests.size());
    std::vector<TransferTask*> tasks;
    for (size_t i = 0; i < requests.size(); ++i) {
        auto& task = batch.task_list[start + i];
        task.batch_id = id;
        task.request = &requests[i];
        tasks.push_back(&task);
    }
    return submitTransferTask(tasks);
}
Status LucaTransport::getTransferStatus(BatchID id, size_t index,
                                        TransferStatus& status) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto& batch = *reinterpret_cast<BatchDesc*>(id);
    if (index >= batch.task_list.size())
        return Status::InvalidArgument(
            "LucaTransport: task index out of range");
    auto& task = batch.task_list[index];
    auto entry = impl_->pending.find(&task);
    if (entry != impl_->pending.end()) {
        DeviceScope device;
        if (device.valid) {
            auto context = entry->second;
            impl_->progress(context);
        }
    }
    auto success = __atomic_load_n(&task.success_slice_count, __ATOMIC_ACQUIRE);
    auto failed = __atomic_load_n(&task.failed_slice_count, __ATOMIC_ACQUIRE);
    status.transferred_bytes =
        __atomic_load_n(&task.transferred_bytes, __ATOMIC_RELAXED);
    if (task.slice_count && success + failed == task.slice_count) {
        status.s = failed ? FAILED : COMPLETED;
        task.is_finished = true;
    } else
        status.s = WAITING;
    return Status::OK();
}
int LucaTransport::registerLocalMemory(void* ptr, size_t length,
                                       const std::string& location,
                                       bool remote_accessible, bool update) {
    (void)remote_accessible;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    DeviceScope device;
    if (!device.valid) return -1;
    return impl_->registerBatch({BufferEntry{ptr, length}}, location, update);
}
int LucaTransport::registerLocalMemoryBatch(
    const std::vector<BufferEntry>& entries, const std::string& location) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    DeviceScope device;
    return device.valid ? impl_->registerBatch(entries, location, true) : -1;
}
int LucaTransport::unregisterLocalMemory(void* ptr, bool update) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    DeviceScope device;
    return device.valid ? impl_->unregisterBatch({ptr}, update) : -1;
}
int LucaTransport::unregisterLocalMemoryBatch(
    const std::vector<void*>& entries) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    DeviceScope device;
    return device.valid ? impl_->unregisterBatch(entries, true) : -1;
}
}  // namespace mooncake
