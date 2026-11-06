#include "FsrNVN.hpp"
#include "fsr_shared.h"
#include <cstdarg>
#include <cstddef>
#include <cstring>

// fsrSwitch module for SaltyNX Core. See FsrNVN.hpp.
//
// Per frame (only when enabled for the game):
//   game queue --fence--> our queue: copy frame -> our texture, draw sharpen shader back into the frame
//   our queue  --fence--> game queue: present
// Nothing is buffered: the effect runs inside the same frame, it only adds its GPU time.

#if defined(__aarch64__)
// Calls fn(arg) with the stack pointer moved to stackTop (present may run on a thread with a tiny stack).
extern "C" uintptr_t fsr_nvn_call_on_stack(uintptr_t (*fn)(void*), void* arg, void* stackTop);
asm(R"(
	.text
	.balign 4
	.global fsr_nvn_call_on_stack
	.hidden fsr_nvn_call_on_stack
	.type fsr_nvn_call_on_stack, %function
fsr_nvn_call_on_stack:
	stp x29, x30, [sp, #-16]!
	mov x29, sp
	mov sp, x2
	mov x9, x0
	mov x0, x1
	blr x9
	mov sp, x29
	ldp x29, x30, [sp], #16
	ret
	.size fsr_nvn_call_on_stack, . - fsr_nvn_call_on_stack
)");
#endif

namespace FsrNVN {
	namespace {
		using namespace NVN;

		// ------------------------------------------------------------------ settings
		constexpr const char* CONFIG_DIR = "sdmc:/config/fsrSwitch";
		constexpr const char* BUILD_TAG = "NX-FSR 0.3";

		struct Config {
			bool enabled = false;  // off until turned on in the overlay (or the .ini)
			int sharpness = 500;   // 0..1000 (shader gets 0.0..1.0), 0 = no sharpening
			int antialias = 0;     // 0..1000, 0 = no anti-aliasing
			bool split = false;    // comparison: effect only on the left half, line in the middle
			int flip = FSR_FLIP_AUTO;
		};
		Config cfg{};
		bool globallyDisabled = false;
		uint64_t titleId = 0;

		// Live settings and status, shared with the overlay (nullptr when SaltyNX had no room for it).
		volatile FsrSharedBlock* shared = nullptr;
		int originMode = -1;   // NVN window origin mode of the game, -1 = unknown

		// SD card access goes through SaltyNX's sysmodule, only possible during FsrSwitch_Init.
		bool ipcOpen = false;

		bool SettingEnabled() { return shared ? shared->enabled != 0 : cfg.enabled; }
		int SettingSharpness() {
			if (!shared) return cfg.sharpness;
			uint32_t v = shared->sharpness;
			return v > 1000 ? 1000 : (int)v;
		}
		int SettingAntialias() {
			if (!shared) return cfg.antialias;
			uint32_t v = shared->antialias;
			return v > 1000 ? 1000 : (int)v;
		}
		bool SettingSplit() { return shared ? shared->split != 0 : cfg.split; }
		int SettingFlip() { return shared ? (int)shared->flip : cfg.flip; }
		void SetState(uint32_t state) { if (shared) shared->state = state; }

		// Frame copy memory, reserved at game start (before the game takes all memory).
		constexpr size_t COPY_MEMORY_WANTED = (size_t)1920 * 1152 * 4 + 0x40000;
		uintptr_t copyMemory = 0;
		size_t copyMemorySize = 0;

		// ------------------------------------------------------------------ log
		// One small log per game: sdmc:/config/fsrSwitch/log_<titleid>.txt, rewritten at a few milestones only.
		char logBuf[0x2000];
		size_t logLen = 0;
		bool logDirty = false;

		void Log(const char* fmt, ...) {
			if (logLen >= sizeof(logBuf) - 2) return;
			va_list args;
			va_start(args, fmt);
			int n = npf_vsnprintf(logBuf + logLen, sizeof(logBuf) - logLen, fmt, args);
			va_end(args);
			if (n > 0) {
				logLen += (size_t)n;
				if (logLen > sizeof(logBuf) - 1) logLen = sizeof(logBuf) - 1;
			}
			logDirty = true;
		}

		void EnsureDir() {
			SaltySDCore_mkdir("sdmc:/config", 0777);
			SaltySDCore_mkdir(CONFIG_DIR, 0777);
		}

		void FlushLog() {
			if (!logDirty || !ipcOpen) return;
			EnsureDir();
			char path[96];
			npf_snprintf(path, sizeof(path), "%s/log_%016lX.txt", CONFIG_DIR, (unsigned long)titleId);
			FILE* f = SaltySDCore_fopen(path, "wb");
			if (!f) return;
			SaltySDCore_fwrite(logBuf, 1, logLen, f);
			SaltySDCore_fclose(f);
			logDirty = false;
		}

		// ------------------------------------------------------------------ config
		// Text file, one "key=value" per line: enabled=1, sharpness=0.50, split=1. '#' starts a comment.
		int ParseMilli(const char* s) {
			// "0.5" / "1" / ".75" -> 500 / 1000 / 750 (no float printf/scanf in Core)
			int whole = 0, frac = 0, scale = 1000;
			while (*s == ' ') s++;
			while (*s >= '0' && *s <= '9') { whole = whole * 10 + (*s - '0'); s++; if (whole > 1000) break; }
			if (*s == '.' || *s == ',') {
				s++;
				while (*s >= '0' && *s <= '9' && scale > 1) { scale /= 10; frac += (*s - '0') * scale; s++; }
			}
			int v = whole * 1000 + frac;
			return v < 0 ? 0 : (v > 1000 ? 1000 : v);
		}

		void ParseConfig(char* text) {
			char* line = text;
			while (line && *line) {
				char* next = strchr(line, '\n');
				if (next) *next++ = 0;
				char* hash = strchr(line, '#');
				if (hash) *hash = 0;
				char* eq = strchr(line, '=');
				if (eq) {
					*eq = 0;
					char* key = line;
					char* value = eq + 1;
					while (*key == ' ' || *key == '\t') key++;
					size_t klen = strlen(key);
					while (klen && (key[klen - 1] == ' ' || key[klen - 1] == '\t' || key[klen - 1] == '\r')) key[--klen] = 0;
					while (*value == ' ' || *value == '\t') value++;
					if (!strcmp(key, "enabled")) cfg.enabled = (*value == '1' || *value == 'y' || *value == 'Y' || *value == 't');
					else if (!strcmp(key, "sharpness")) cfg.sharpness = ParseMilli(value);
					else if (!strcmp(key, "antialias")) cfg.antialias = ParseMilli(value);
					else if (!strcmp(key, "split")) cfg.split = (*value == '1' || *value == 'y' || *value == 'Y' || *value == 't');
					else if (!strcmp(key, "flip")) {
						if (*value == 'a' || *value == 'A') cfg.flip = FSR_FLIP_AUTO;
						else if (*value == '1' || (*value == 'o' && value[1] == 'n') || (*value == 'O' && value[1] == 'N')) cfg.flip = FSR_FLIP_ON;
						else cfg.flip = FSR_FLIP_OFF;
					}
				}
				line = next;
			}
		}

		bool ReadFile(const char* path, char* buf, size_t size) {
			FILE* f = SaltySDCore_fopen(path, "rb");
			if (!f) return false;
			size_t n = SaltySDCore_fread(buf, 1, size - 1, f);
			SaltySDCore_fclose(f);
			buf[n] = 0;
			return true;
		}

		// ------------------------------------------------------------------ NVN functions
		constexpr int STAGE_VERTEX = 0;
		constexpr int STAGE_FRAGMENT = 1;
		constexpr int UBO_BINDING = 0;
		constexpr int TEXTURE_BINDING = 0;
		constexpr int MEMORY_POOL_FLAGS_CPU_NO_ACCESS = 0x1;
		constexpr int MEMORY_POOL_FLAGS_CPU_UNCACHED = 0x2;
		constexpr int MEMORY_POOL_FLAGS_GPU_CACHED = 0x20;
		constexpr int MEMORY_POOL_FLAGS_SHADER_CODE = 0x40;
		constexpr int BARRIER_ORDER_PRIMITIVES = 0x1;
		constexpr int BARRIER_ORDER_FRAGMENTS = 0x2;
		constexpr int BARRIER_INVALIDATE_TEXTURE = 0x10;
		constexpr int BARRIER_INVALIDATE_TEXTURE_DESCRIPTOR = 0x40;
		constexpr int TEXTURE_TARGET_2D = 1;
		constexpr int COUNTER_TYPE_TIMESTAMP = 0;
		constexpr int DRAW_MODE_TRIANGLES = 4;

		struct CounterData {
			uint64_t value;
			uint64_t timestamp;
		};

		#define FSR_NVN_FUNCTIONS(X) \
			X(DeviceGetInteger, void, const Device* device, int info, int* out) \
			X(DeviceGetTextureHandle, TextureHandle, const Device* device, int textureId, int samplerId) \
			X(MemoryPoolBuilderSetDefaults, void, const MemoryPoolBuilder* builder) \
			X(MemoryPoolBuilderSetDevice, void, const MemoryPoolBuilder* builder, const Device* device) \
			X(MemoryPoolBuilderSetFlags, void, const MemoryPoolBuilder* builder, int flags) \
			X(MemoryPoolBuilderSetStorage, void, const MemoryPoolBuilder* builder, void* buffer, size_t size) \
			X(MemoryPoolInitialize, bool, const MemoryPool* pool, const MemoryPoolBuilder* builder) \
			X(MemoryPoolMap, void*, const MemoryPool* pool) \
			X(MemoryPoolGetBufferAddress, BufferAddress, const MemoryPool* pool) \
			X(ProgramInitialize, bool, Program* program, Device* device) \
			X(ProgramSetShaders, bool, Program* program, int count, const ShaderData* shaders) \
			X(DepthStencilStateSetDefaults, void, DepthStencilState* state) \
			X(ChannelMaskStateSetDefaults, void, ChannelMaskState* state) \
			X(ColorStateSetDefaults, void, ColorState* state) \
			X(BlendStateSetDefaults, void, BlendState* state) \
			X(PolygonStateSetDefaults, void, PolygonState* state) \
			X(TexturePoolInitialize, bool, TexturePool* pool, const MemoryPool* memoryPool, ptrdiff_t offset, int numDescriptors) \
			X(TexturePoolRegisterTexture, void, const TexturePool* pool, int id, const Texture* texture, const TextureView* view) \
			X(SamplerPoolInitialize, bool, SamplerPool* pool, const MemoryPool* memoryPool, ptrdiff_t offset, int numDescriptors) \
			X(SamplerPoolRegisterSampler, void, const SamplerPool* pool, int id, const Sampler* sampler) \
			X(SamplerBuilderSetDevice, void, SamplerBuilder* builder, Device* device) \
			X(SamplerBuilderSetDefaults, void, SamplerBuilder* builder) \
			X(SamplerBuilderSetMinMagFilter, void, SamplerBuilder* builder, int min, int mag) \
			X(SamplerBuilderSetWrapMode, void, SamplerBuilder* builder, int s, int t, int r) \
			X(SamplerInitialize, bool, Sampler* sampler, const SamplerBuilder* builder) \
			X(TextureBuilderSetDevice, void, TextureBuilder* builder, Device* device) \
			X(TextureBuilderSetDefaults, void, TextureBuilder* builder) \
			X(TextureBuilderSetTarget, void, TextureBuilder* builder, int target) \
			X(TextureBuilderSetFormat, void, TextureBuilder* builder, int format) \
			X(TextureBuilderSetSize2D, void, TextureBuilder* builder, int width, int height) \
			X(TextureBuilderGetStorageSize, size_t, const TextureBuilder* builder) \
			X(TextureBuilderGetStorageAlignment, size_t, const TextureBuilder* builder) \
			X(TextureBuilderSetStorage, void, TextureBuilder* builder, const MemoryPool* pool, ptrdiff_t offset) \
			X(TextureInitialize, bool, Texture* texture, const TextureBuilder* builder) \
			X(TextureFinalize, void, Texture* texture) \
			X(TextureGetWidth, int, const Texture* texture) \
			X(TextureGetHeight, int, const Texture* texture) \
			X(TextureGetFormat, int, const Texture* texture) \
			X(CommandBufferInitialize, bool, const CommandBuffer* cmdBuf, Device* device) \
			X(CommandBufferAddCommandMemory, void, const CommandBuffer* cmdBuf, const MemoryPool* pool, ptrdiff_t offset, size_t size) \
			X(CommandBufferAddControlMemory, void, const CommandBuffer* cmdBuf, void* buffer, size_t size) \
			X(CommandBufferBeginRecording, void, const CommandBuffer* cmdBuf) \
			X(CommandBufferEndRecording, CommandHandle, const CommandBuffer* cmdBuf) \
			X(CommandBufferSetRenderTargets, void, CommandBuffer* cmdBuf, int numTextures, const Texture** textures, const TextureView** views, const Texture* depth, const TextureView* depthView) \
			X(CommandBufferSetViewport, void, CommandBuffer* cmdBuf, int x, int y, int width, int height) \
			X(CommandBufferSetScissor, void, CommandBuffer* cmdBuf, int x, int y, int width, int height) \
			X(CommandBufferBarrier, void, const CommandBuffer* cmdBuf, int barrier) \
			X(CommandBufferSetTexturePool, void, const CommandBuffer* cmdBuf, const TexturePool* pool) \
			X(CommandBufferSetSamplerPool, void, const CommandBuffer* cmdBuf, const SamplerPool* pool) \
			X(CommandBufferBindDepthStencilState, void, const CommandBuffer* cmdBuf, const DepthStencilState* state) \
			X(CommandBufferBindChannelMaskState, void, const CommandBuffer* cmdBuf, const ChannelMaskState* state) \
			X(CommandBufferBindColorState, void, const CommandBuffer* cmdBuf, const ColorState* state) \
			X(CommandBufferBindBlendState, void, const CommandBuffer* cmdBuf, const BlendState* state) \
			X(CommandBufferBindPolygonState, void, const CommandBuffer* cmdBuf, const PolygonState* state) \
			X(CommandBufferBindProgram, void, const CommandBuffer* cmdBuf, const Program* program, int stages) \
			X(CommandBufferBindUniformBuffer, void, const CommandBuffer* cmdBuf, int stage, int index, BufferAddress address, size_t size) \
			X(CommandBufferBindTexture, void, const CommandBuffer* cmdBuf, int stage, int index, TextureHandle handle) \
			X(CommandBufferDrawArrays, void, const CommandBuffer* cmdBuf, int mode, int first, int count) \
			X(CommandBufferCopyTextureToTexture, void, const CommandBuffer* cmdBuf, const Texture* src, const TextureView* srcView, const CopyRegion* srcRegion, const Texture* dst, const TextureView* dstView, const CopyRegion* dstRegion, int flags) \
			X(CommandBufferReportCounter, void, const CommandBuffer* cmdBuf, int type, BufferAddress address) \
			X(QueueSubmitCommands, void, const Queue* queue, int numCommandBuffers, const CommandHandle* handles) \
			X(QueueBuilderSetDevice, void, QueueBuilder* builder, Device* device) \
			X(QueueBuilderSetDefaults, void, QueueBuilder* builder) \
			X(QueueInitialize, bool, Queue* queue, const QueueBuilder* builder) \
			X(QueueFenceSync, void, const Queue* queue, Sync* sync, int condition, int flags) \
			X(QueueWaitSync, void, const Queue* queue, const Sync* sync) \
			X(QueueFlush, void, const Queue* queue) \
			X(SyncInitialize, bool, Sync* sync, Device* device)

		#define FSR_NVN_MEMBER(name, ret, ...) ret (*name)(__VA_ARGS__);
		struct Functions {
			FSR_NVN_FUNCTIONS(FSR_NVN_MEMBER)
		} nvn{};
		#undef FSR_NVN_MEMBER

		#define FSR_NVN_NAME(name, ...) "nvn" #name "\0"
		constexpr char kFunctionNames[] = FSR_NVN_FUNCTIONS(FSR_NVN_NAME);
		#undef FSR_NVN_NAME
		constexpr size_t CountNames(const char* names, size_t size) {
			size_t count = 0;
			for (size_t i = 0; i + 1 < size; i++) if (names[i] == '\0') count++;
			return count;
		}
		static_assert(CountNames(kFunctionNames, sizeof(kFunctionNames)) == sizeof(Functions) / sizeof(void*),
			"kFunctionNames must name every member of Functions");

		// Optional (not in every SDK).
		uint64_t (*DeviceGetTimestampInNanoseconds)(const Device* device, const CounterData* data) = nullptr;
		void (*QueueBuilderSetComputeMemorySize)(QueueBuilder* builder, size_t size) = nullptr;
		size_t (*QueueBuilderGetQueueMemorySize)(const QueueBuilder* builder) = nullptr;
		void (*QueueBuilderSetQueueMemory)(QueueBuilder* builder, void* memory, size_t size) = nullptr;

		const char* missingFunction = nullptr;

		bool loadFunctions(Resolver resolve) {
			const char* name = kFunctionNames;
			for (size_t i = 0; i < sizeof(Functions) / sizeof(void*); i++, name += strlen(name) + 1) {
				void* address = resolve(name);
				if (!address) {
					missingFunction = name;
					return false;
				}
				memcpy(reinterpret_cast<char*>(&nvn) + i * sizeof(void*), &address, sizeof(void*));
			}
			return true;
		}

		template <typename T> void loadOptional(T& function, Resolver resolve, const char* name) {
			void* address = resolve(name);
			memcpy(&function, &address, sizeof(address));
		}

		// ------------------------------------------------------------------ shaders
		// Generated by CI: shaders/*.vert|frag -> UAM -> tools/dksh2nvn.py -> fsr/*.bin
		constexpr char vertexDataStorage[] {
			#embed "../../fsr/vert.code.bin"
		};
		constexpr char fragmentDataStorage[] {
			#embed "../../fsr/frag.code.bin"
		};
		char vertexControlStorage[] {
			#embed "../../fsr/vert.control.bin"
		};
		char fragmentControlStorage[] {
			#embed "../../fsr/frag.control.bin"
		};
		constexpr size_t fragmentDataOffset = (sizeof(vertexDataStorage) + 0xFF) & ~0xFF;
		constexpr size_t shaderDataSize = (fragmentDataOffset + sizeof(fragmentDataStorage) + 0xFFF) & ~0xFFF;
		struct ShaderCode {
			char vertex[fragmentDataOffset];
			char fragment[shaderDataSize - fragmentDataOffset];
		};
		alignas(0x1000) ShaderCode shaderDataStorage {
			{
				#embed "../../fsr/vert.code.bin"
			},
			{
				#embed "../../fsr/frag.code.bin"
			},
		};
		static_assert(sizeof(ShaderCode) == shaderDataSize && offsetof(ShaderCode, fragment) == fragmentDataOffset);

		struct Params {
			float crop[4];    // x, y, width, height
			float effect[4];  // sharpness, split, flip rows, anti-aliasing
		};
		static_assert(sizeof(Params) == 32, "Params must match the std140 layout of the GLSL block");

		// ------------------------------------------------------------------ state
		constexpr int SLOTS = 8;
		constexpr size_t SLOT_SIZE = 0x1000;

		MemoryPool shaderPool{};
		bool shaderPoolReady = false;
		Program program{};
		MemoryPool cmdMemPool{};
		alignas(0x1000) char cmdHostStorage[SLOTS * SLOT_SIZE]{};
		alignas(0x1000) char controlMemStorage[SLOTS * SLOT_SIZE]{};
		CommandBuffer cmdBuf{};

		DepthStencilState depthState{};
		ChannelMaskState channelState{};
		ColorState colorState{};
		BlendState blendState{};
		PolygonState polygonState{};

		constexpr size_t TEX_POOL_OFFSET = 0x0000;
		constexpr size_t SAMPLER_POOL_OFFSET = 0x4000;
		constexpr size_t UBO_POOL_OFFSET = 0x8000;
		constexpr size_t COUNTER_POOL_OFFSET = 0x8C00;
		constexpr size_t RES_POOL_SIZE = 0x9000;
		alignas(0x1000) char resHostStorage[RES_POOL_SIZE]{};
		MemoryPool resPool{};
		TexturePool texturePool{};
		SamplerPool samplerPool{};
		Sampler sourceSampler{};
		uint8_t* resCpu = nullptr;
		BufferAddress resGpu = 0;
		size_t uboStride = 0x100;
		int reservedTextureDescriptors = 256;
		int reservedSamplerDescriptors = 256;

		MemoryPool copyPool{};
		bool copyPoolReady = false;
		Texture copyTexture{};
		bool copyTextureReady = false;
		int copyWidth = 0, copyHeight = 0, copyFormat = 0;
		TextureHandle copyHandle = 0;

		constexpr size_t QUEUE_MEMORY_SIZE = 0x40000;
		alignas(0x1000) char queueMemory[QUEUE_MEMORY_SIZE];
		Queue fsrQueue{};
		Sync gameDone[SLOTS]{};
		Sync fsrDone[SLOTS]{};

		bool initAttempted = false;
		bool ready = false;
		bool failed = false;
		int currentSlot = 0;
		uint64_t frames = 0;
		bool slotUsed[SLOTS]{};
		uint64_t gpuNsAvg = 0;

		bool initPool(MemoryPool* pool, Device* device, int flags, void* storage, size_t size) {
			MemoryPoolBuilder b{};
			nvn.MemoryPoolBuilderSetDefaults(&b);
			nvn.MemoryPoolBuilderSetDevice(&b, device);
			nvn.MemoryPoolBuilderSetFlags(&b, flags);
			nvn.MemoryPoolBuilderSetStorage(&b, storage, size);
			return nvn.MemoryPoolInitialize(pool, &b);
		}

		bool createShaderPool(Device* device) {
			if (!shaderPoolReady)
				shaderPoolReady = initPool(&shaderPool, device, MEMORY_POOL_FLAGS_CPU_UNCACHED | MEMORY_POOL_FLAGS_GPU_CACHED | MEMORY_POOL_FLAGS_SHADER_CODE,
				                           &shaderDataStorage, sizeof(shaderDataStorage));
			return shaderPoolReady;
		}

		bool buildResources(Device* device) {
			int uboAlign = 0x100, texDescSize = 0x20, samplerDescSize = 0x20;
			nvn.DeviceGetInteger(device, 4, &uboAlign);
			nvn.DeviceGetInteger(device, 15, &texDescSize);
			nvn.DeviceGetInteger(device, 16, &samplerDescSize);
			nvn.DeviceGetInteger(device, 17, &reservedTextureDescriptors);
			nvn.DeviceGetInteger(device, 18, &reservedSamplerDescriptors);
			if (uboAlign < 0x100) uboAlign = 0x100;
			uboStride = (size_t)uboAlign;
			Log("device: ubo align 0x%x, tex desc 0x%x, sampler desc 0x%x, reserved tex %d, reserved samplers %d\n",
			    uboAlign, texDescSize, samplerDescSize, reservedTextureDescriptors, reservedSamplerDescriptors);

			const size_t texPoolBytes = (size_t)(reservedTextureDescriptors + 1) * (size_t)texDescSize;
			const size_t samplerPoolBytes = (size_t)(reservedSamplerDescriptors + 1) * (size_t)samplerDescSize;
			if (texPoolBytes > SAMPLER_POOL_OFFSET - TEX_POOL_OFFSET) return false;
			if (samplerPoolBytes > UBO_POOL_OFFSET - SAMPLER_POOL_OFFSET) return false;
			if (uboStride * SLOTS > COUNTER_POOL_OFFSET - UBO_POOL_OFFSET) return false;

			if (!initPool(&resPool, device, MEMORY_POOL_FLAGS_CPU_UNCACHED | MEMORY_POOL_FLAGS_GPU_CACHED, resHostStorage, sizeof(resHostStorage))) return false;
			resCpu = (uint8_t*)nvn.MemoryPoolMap(&resPool);
			if (!resCpu) return false;
			resGpu = nvn.MemoryPoolGetBufferAddress(&resPool);

			SamplerBuilder sb{};
			nvn.SamplerBuilderSetDefaults(&sb);
			nvn.SamplerBuilderSetDevice(&sb, device);
			nvn.SamplerBuilderSetMinMagFilter(&sb, 1, 1); // linear: FXAA samples between texels
			nvn.SamplerBuilderSetWrapMode(&sb, 0, 0, 0);
			if (!nvn.SamplerInitialize(&sourceSampler, &sb)) return false;
			if (!nvn.SamplerPoolInitialize(&samplerPool, &resPool, (ptrdiff_t)SAMPLER_POOL_OFFSET, reservedSamplerDescriptors + 1)) return false;
			nvn.SamplerPoolRegisterSampler(&samplerPool, reservedSamplerDescriptors, &sourceSampler);
			return nvn.TexturePoolInitialize(&texturePool, &resPool, (ptrdiff_t)TEX_POOL_OFFSET, reservedTextureDescriptors + 1);
		}

		bool buildProgram(Device* device) {
			if (!createShaderPool(device)) return false;
			const BufferAddress base = nvn.MemoryPoolGetBufferAddress(&shaderPool);
			if (!nvn.ProgramInitialize(&program, device)) return false;
			const ShaderData stages[2] = {
				{ base, vertexControlStorage },
				{ base + fragmentDataOffset, fragmentControlStorage },
			};
			if (!nvn.ProgramSetShaders(&program, 2, stages)) return false;
			nvn.DepthStencilStateSetDefaults(&depthState);
			nvn.ChannelMaskStateSetDefaults(&channelState);
			nvn.ColorStateSetDefaults(&colorState);
			nvn.BlendStateSetDefaults(&blendState);
			nvn.PolygonStateSetDefaults(&polygonState);
			return true;
		}

		bool buildQueue(Device* device, Resolver resolve) {
			loadOptional(QueueBuilderSetComputeMemorySize, resolve, "nvnQueueBuilderSetComputeMemorySize");
			loadOptional(QueueBuilderGetQueueMemorySize, resolve, "nvnQueueBuilderGetQueueMemorySize");
			loadOptional(QueueBuilderSetQueueMemory, resolve, "nvnQueueBuilderSetQueueMemory");
			QueueBuilder qb{};
			nvn.QueueBuilderSetDevice(&qb, device);
			nvn.QueueBuilderSetDefaults(&qb);
			if (QueueBuilderSetComputeMemorySize) QueueBuilderSetComputeMemorySize(&qb, 0);
			if (QueueBuilderGetQueueMemorySize && QueueBuilderSetQueueMemory) {
				const size_t size = (QueueBuilderGetQueueMemorySize(&qb) + 0xFFF) & ~(size_t)0xFFF;
				if (!size || size > QUEUE_MEMORY_SIZE) {
					Log("queue: needs 0x%lx bytes, have 0x%lx\n", (unsigned long)size, (unsigned long)QUEUE_MEMORY_SIZE);
					return false;
				}
				QueueBuilderSetQueueMemory(&qb, queueMemory, size);
			}
			if (!nvn.QueueInitialize(&fsrQueue, &qb)) return false;
			for (int i = 0; i < SLOTS; i++)
				if (!nvn.SyncInitialize(&gameDone[i], device) || !nvn.SyncInitialize(&fsrDone[i], device)) return false;
			return true;
		}

		bool init(Device* device, Resolver resolve) {
			if (initAttempted) return ready;
			initAttempted = true;
			const char* stage = nullptr;
			if (!device) stage = "no device";
			else if (!loadFunctions(resolve)) stage = "functions";
			else if (!initPool(&cmdMemPool, device, MEMORY_POOL_FLAGS_CPU_UNCACHED | MEMORY_POOL_FLAGS_GPU_CACHED, cmdHostStorage, sizeof(cmdHostStorage))
			         || !nvn.CommandBufferInitialize(&cmdBuf, device)) stage = "command buffer";
			else if (!buildResources(device)) stage = "resources";
			else if (!buildProgram(device)) stage = "shader program (NVN rejected our shaders)";
			else if (!buildQueue(device, resolve)) stage = "own queue";
			else if (!copyMemory) stage = "no memory for frame copy";
			else if (!(copyPoolReady = initPool(&copyPool, device, MEMORY_POOL_FLAGS_CPU_NO_ACCESS | MEMORY_POOL_FLAGS_GPU_CACHED, (void*)copyMemory, copyMemorySize))) stage = "frame copy memory pool";
			if (stage == nullptr) loadOptional(DeviceGetTimestampInNanoseconds, resolve, "nvnDeviceGetTimestampInNanoseconds");
			ready = (stage == nullptr);
			if (ready) Log("init: OK (vert 0x%lx bytes, frag 0x%lx bytes, shader pool %s)\n",
			               (unsigned long)sizeof(vertexDataStorage), (unsigned long)sizeof(fragmentDataStorage),
			               shaderPoolReady ? "ready" : "?");
			else Log("init: FAILED at %s%s%s\n", stage, missingFunction ? ", missing " : "", missingFunction ? missingFunction : "");
			FlushLog();
			return ready;
		}

		bool prepareCopyTexture(Device* device, int width, int height, int format) {
			if (copyTextureReady && width == copyWidth && height == copyHeight && format == copyFormat) return true;
			if (copyTextureReady) {
				nvn.TextureFinalize(&copyTexture);
				copyTextureReady = false;
			}
			TextureBuilder tb{};
			nvn.TextureBuilderSetDevice(&tb, device);
			nvn.TextureBuilderSetDefaults(&tb);
			nvn.TextureBuilderSetTarget(&tb, TEXTURE_TARGET_2D);
			nvn.TextureBuilderSetFormat(&tb, format);
			nvn.TextureBuilderSetSize2D(&tb, width, height);
			const size_t storage = nvn.TextureBuilderGetStorageSize(&tb);
			const size_t alignment = nvn.TextureBuilderGetStorageAlignment(&tb);
			Log("copy texture: %dx%d format %d, storage 0x%lx, alignment 0x%lx, have 0x%lx\n",
			    width, height, format, (unsigned long)storage, (unsigned long)alignment, (unsigned long)copyMemorySize);
			if (!storage || storage > copyMemorySize) return false;
			nvn.TextureBuilderSetStorage(&tb, &copyPool, 0);
			if (!nvn.TextureInitialize(&copyTexture, &tb)) return false;
			copyTextureReady = true;
			copyWidth = width;
			copyHeight = height;
			copyFormat = format;
			const int textureId = reservedTextureDescriptors;
			nvn.TexturePoolRegisterTexture(&texturePool, textureId, &copyTexture, nullptr);
			copyHandle = nvn.DeviceGetTextureHandle(device, textureId, reservedSamplerDescriptors);
			return true;
		}

		CounterData* counterCpu(int slot, int which) {
			return (CounterData*)(resCpu + COUNTER_POOL_OFFSET + (size_t)slot * 32 + (size_t)which * 16);
		}
		BufferAddress counterGpu(int slot, int which) {
			return resGpu + COUNTER_POOL_OFFSET + (BufferAddress)slot * 32 + (BufferAddress)which * 16;
		}

		void collectTiming(Device* device, int slot) {
			if (!slotUsed[slot]) return;
			const CounterData* begin = counterCpu(slot, 0);
			const CounterData* end = counterCpu(slot, 1);
			uint64_t b = begin->timestamp, e = end->timestamp;
			if (DeviceGetTimestampInNanoseconds) {
				b = DeviceGetTimestampInNanoseconds(device, begin);
				e = DeviceGetTimestampInNanoseconds(device, end);
			}
			if (e > b && e - b < 100000000ull) { // ignore garbage (> 100 ms)
				// Moving average, shown by the overlay.
				gpuNsAvg = gpuNsAvg ? (gpuNsAvg * 15 + (e - b)) / 16 : (e - b);
				if (shared) shared->gpuTimeNs = (uint32_t)gpuNsAvg;
			}
		}

		bool FlipRows() {
			const int flip = SettingFlip();
			if (flip == FSR_FLIP_ON) return true;
			if (flip == FSR_FLIP_OFF) return false;
			return originMode == 0; // NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT: rows run bottom to top
		}

		bool Run(Device* device, const Queue* queue, const Texture* target, const Rectangle& crop, Resolver resolve) {
			if (!init(device, resolve)) return false;

			const int width = nvn.TextureGetWidth(target);
			const int height = nvn.TextureGetHeight(target);
			const int format = nvn.TextureGetFormat(target);
			if (width <= 0 || height <= 0) return true;
			if (!prepareCopyTexture(device, width, height, format)) return false;

			const bool haveCrop = crop.width > 0 && crop.height > 0;
			const int visibleW = haveCrop ? crop.width : width;
			const int visibleH = haveCrop ? crop.height : height;
			const int originX = haveCrop ? crop.x : 0;
			const int originY = haveCrop ? crop.y : 0;
			const bool flipRows = FlipRows();

			const int slot = currentSlot;
			currentSlot = (currentSlot + 1) % SLOTS;
			collectTiming(device, slot);

			// Our queue starts after everything the game submitted so far (the frame is complete).
			nvn.QueueFenceSync(queue, &gameDone[slot], 0, 0);
			nvn.QueueFlush(queue);
			nvn.QueueWaitSync(&fsrQueue, &gameDone[slot]);

			nvn.CommandBufferAddCommandMemory(&cmdBuf, &cmdMemPool, (ptrdiff_t)(slot * SLOT_SIZE), SLOT_SIZE);
			nvn.CommandBufferAddControlMemory(&cmdBuf, controlMemStorage + slot * SLOT_SIZE, SLOT_SIZE);

			Params params{};
			params.crop[0] = (float)originX;
			params.crop[1] = (float)originY;
			params.crop[2] = (float)visibleW;
			params.crop[3] = (float)visibleH;
			params.effect[0] = (float)SettingSharpness() / 1000.0f;
			params.effect[1] = SettingSplit() ? 1.0f : 0.0f;
			params.effect[2] = flipRows ? 1.0f : 0.0f;
			params.effect[3] = (float)SettingAntialias() / 1000.0f;
			memcpy(resCpu + UBO_POOL_OFFSET + (size_t)slot * uboStride, &params, sizeof(params));
			const BufferAddress paramsAddress = resGpu + UBO_POOL_OFFSET + (BufferAddress)slot * uboStride;

			CopyRegion region{originX, originY, 0, visibleW, visibleH, 1};

			nvn.CommandBufferBeginRecording(&cmdBuf);
			nvn.CommandBufferReportCounter(&cmdBuf, COUNTER_TYPE_TIMESTAMP, counterGpu(slot, 0));
			nvn.CommandBufferBarrier(&cmdBuf, BARRIER_ORDER_FRAGMENTS);
			nvn.CommandBufferCopyTextureToTexture(&cmdBuf, target, nullptr, &region, &copyTexture, nullptr, &region, 0);
			nvn.CommandBufferBarrier(&cmdBuf, BARRIER_ORDER_PRIMITIVES | BARRIER_ORDER_FRAGMENTS | BARRIER_INVALIDATE_TEXTURE | BARRIER_INVALIDATE_TEXTURE_DESCRIPTOR);
			nvn.CommandBufferSetRenderTargets(&cmdBuf, 1, &target, nullptr, nullptr, nullptr);
			nvn.CommandBufferSetViewport(&cmdBuf, originX, originY, visibleW, visibleH);
			nvn.CommandBufferSetScissor(&cmdBuf, originX, originY, visibleW, visibleH);
			nvn.CommandBufferSetTexturePool(&cmdBuf, &texturePool);
			nvn.CommandBufferSetSamplerPool(&cmdBuf, &samplerPool);
			nvn.CommandBufferBindDepthStencilState(&cmdBuf, &depthState);
			nvn.CommandBufferBindChannelMaskState(&cmdBuf, &channelState);
			nvn.CommandBufferBindColorState(&cmdBuf, &colorState);
			nvn.CommandBufferBindBlendState(&cmdBuf, &blendState);
			nvn.CommandBufferBindPolygonState(&cmdBuf, &polygonState);
			nvn.CommandBufferBindProgram(&cmdBuf, &program, 0x1F);
			nvn.CommandBufferBindUniformBuffer(&cmdBuf, STAGE_VERTEX, UBO_BINDING, paramsAddress, sizeof(Params));
			nvn.CommandBufferBindUniformBuffer(&cmdBuf, STAGE_FRAGMENT, UBO_BINDING, paramsAddress, sizeof(Params));
			nvn.CommandBufferBindTexture(&cmdBuf, STAGE_FRAGMENT, TEXTURE_BINDING, copyHandle);
			nvn.CommandBufferDrawArrays(&cmdBuf, DRAW_MODE_TRIANGLES, 0, 3);
			nvn.CommandBufferReportCounter(&cmdBuf, COUNTER_TYPE_TIMESTAMP, counterGpu(slot, 1));
			const CommandHandle handle = nvn.CommandBufferEndRecording(&cmdBuf);
			nvn.QueueSubmitCommands(&fsrQueue, 1, &handle);

			// Game's queue (present comes next) waits until our pass is done.
			nvn.QueueFenceSync(&fsrQueue, &fsrDone[slot], 0, 0);
			nvn.QueueFlush(&fsrQueue);
			nvn.QueueWaitSync(queue, &fsrDone[slot]);
			slotUsed[slot] = true;

			frames++;
			if (shared) {
				shared->state = FSR_STATE_RUNNING;
				shared->width = (uint32_t)width;
				shared->height = (uint32_t)height;
				shared->format = (uint32_t)format;
				shared->originMode = (uint32_t)originMode;
				shared->flipActive = flipRows ? 1 : 0;
				shared->frames = (uint32_t)frames;
			}
			return true;
		}

		struct RunArgs {
			Device* device; const Queue* queue; const Texture* target; const Rectangle* crop; Resolver resolve;
		};
		uintptr_t RunTrampoline(void* p) {
			const RunArgs& a = *(const RunArgs*)p;
			return Run(a.device, a.queue, a.target, *a.crop, a.resolve) ? 1 : 0;
		}
		alignas(0x10) char runStack[0x10000];
	}

	bool Wanted() {
		return !globallyDisabled && copyMemory != 0 && !failed;
	}

	void PrepareEarly(Device* device, const DeviceBuilder* builder, Resolver resolve) {
		if (!Wanted() || initAttempted || !device) return;
		// Where the game's frame starts in memory (upper or lower left), see FlipRows().
		int (*DeviceGetWindowOriginMode)(const Device*) = nullptr;
		int (*DeviceBuilderGetWindowOriginMode)(const DeviceBuilder*) = nullptr;
		loadOptional(DeviceGetWindowOriginMode, resolve, "nvnDeviceGetWindowOriginMode");
		loadOptional(DeviceBuilderGetWindowOriginMode, resolve, "nvnDeviceBuilderGetWindowOriginMode");
		if (DeviceGetWindowOriginMode) originMode = DeviceGetWindowOriginMode(device);
		else if (DeviceBuilderGetWindowOriginMode && builder) originMode = DeviceBuilderGetWindowOriginMode(builder);
		if (shared) shared->originMode = (uint32_t)originMode;
		if (!shaderPoolReady && loadFunctions(resolve)) createShaderPool(device);
	}

	void Process(Device* device, const Queue* queue, const Texture* target, const Rectangle& crop, Resolver resolve) {
		if (!Wanted() || !SettingEnabled() || !device || !queue || !target) return;
		if (SettingSharpness() == 0 && SettingAntialias() == 0) return; // nothing to do
		RunArgs args{device, queue, target, &crop, resolve};
#if defined(__aarch64__)
		const bool ok = fsr_nvn_call_on_stack(RunTrampoline, &args, runStack + sizeof(runStack)) != 0;
#else
		const bool ok = RunTrampoline(&args) != 0;
#endif
		if (!ok) {
			failed = true;
			SetState(FSR_STATE_FAILED);
		}
	}
}

extern "C" void FsrSwitch_Init(SharedMemory* sharedMemory) {
	using namespace FsrNVN;
	ipcOpen = true;
	svcGetInfo(&titleId, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0);
	Log("%s, SaltyNX Core " APP_VERSION ", title %016lX\n", BUILD_TAG, (unsigned long)titleId);

	char text[1024];
	char path[96];
	npf_snprintf(path, sizeof(path), "%s/disabled.flag", CONFIG_DIR);
	if (FILE* f = SaltySDCore_fopen(path, "rb")) {
		SaltySDCore_fclose(f);
		globallyDisabled = true;
		Log("disabled.flag found: off for all games\n");
		FlushLog();
		ipcOpen = false;
		return;
	}
	npf_snprintf(path, sizeof(path), "%s/%016lX.ini", CONFIG_DIR, (unsigned long)titleId);
	const bool haveConfig = ReadFile(path, text, sizeof(text));
	if (haveConfig) {
		ParseConfig(text);
		Log("config: %s\n", path);
	}
	else Log("config: none (off; turn it on in the overlay)\n");
	Log("settings: enabled %d, sharpness %d/1000, anti-aliasing %d/1000, split %d, flip %d\n",
	    cfg.enabled ? 1 : 0, cfg.sharpness, cfg.antialias, cfg.split ? 1 : 0, cfg.flip);

	// Live settings / status block for the overlay.
	ptrdiff_t offset = 0;
	if (sharedMemory && !SaltySD_CheckIfSharedMemoryAvailable(&offset, sizeof(FsrSharedBlock))) {
		shared = (volatile FsrSharedBlock*)((uintptr_t)shmemGetAddr(sharedMemory) + offset);
		shared->version = FSR_SHARED_VERSION;
		shared->titleIdLow = (uint32_t)titleId;
		shared->titleIdHigh = (uint32_t)(titleId >> 32);
		shared->enabled = cfg.enabled ? 1 : 0;
		shared->sharpness = (uint32_t)cfg.sharpness;
		shared->split = cfg.split ? 1 : 0;
		shared->flip = (uint32_t)cfg.flip;
		shared->antialias = (uint32_t)cfg.antialias;
		shared->state = FSR_STATE_WAITING;
		shared->width = 0;
		shared->height = 0;
		shared->format = 0;
		shared->originMode = 0xFFFFFFFFu;
		shared->flipActive = 0;
		shared->gpuTimeNs = 0;
		shared->frames = 0;
		shared->magic = FSR_SHARED_MAGIC;
		Log("shared memory: block at offset 0x%lx\n", (unsigned long)offset);
	}
	else Log("shared memory: no room, overlay can't change settings in game\n");

	if (cfg.enabled) {
		copyMemory = SaltySDCore_ReserveMemoryAppend(COPY_MEMORY_WANTED);
		copyMemorySize = (COPY_MEMORY_WANTED + 0x1FFFFF) & ~(size_t)0x1FFFFF;
		if (copyMemory) Log("memory: reserved 0x%lx bytes for frame copy\n", (unsigned long)copyMemorySize);
		else Log("memory: reservation FAILED, effect off\n");
	}
	if (!copyMemory) SetState(FSR_STATE_NO_MEMORY);
	Log("waiting for the game's first NVN frame (frame info and GPU time are shown in the overlay)\n");
	// Games never set up in the overlay leave nothing on the SD card.
	if (haveConfig) FlushLog();
	ipcOpen = false;
}
