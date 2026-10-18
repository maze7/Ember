// Boot and shutdown: the loader, the instance, the adapter, the device with its queues and their
// timelines, VMA, the pools and the slots. Everything here runs once, on main's fiber. boot() may
// stop half way, and teardown() takes down whatever exists, in reverse.

#include <ember/gpu/gpu.h>

#include <ember/containers/string.h>
#include <ember/containers/vector.h>
#include <ember/core/frame.h>
#include <ember/core/jobs.h>
#include <ember/memory/alignment.h>
#include <ember/memory/heap.h>
#include <ember/memory/new.h>
#include <gpu/vulkan/backend.h>

#if defined(EMBER_HAS_PLATFORM)
	#include <platform/wsi.h>
#endif

#include <cstdio>
#include <cstring>
#include <new>

namespace ember::gpu
{
	namespace
	{
		// The one backend. A pointer, so the static holds nothing.
		vk::Backend* s_backend = nullptr;

		// Validation messages since initialize(). Process statics, since a test reads them once the
		// device is gone.
		std::atomic<u32> s_validation_errors{0};
		std::atomic<u32> s_validation_warnings{0};
		bool s_break_on_validation_error = false;

		constexpr const char* VALIDATION_LAYER = "VK_LAYER_KHRONOS_validation";
		constexpr u32 MAX_ENABLED_NAMES = 16;

		// Vulkan's and VMA's CPU memory, from the heap under Tag::Graphics. A reallocation at an
		// alignment the heap's reallocate cannot keep goes through a fresh block.
		void* VKAPI_PTR allocate_callback(void*, size_t size, size_t alignment,
		    VkSystemAllocationScope)
		{
			return memory::allocate(memory::Tag::Graphics, size,
			    alignment > DEFAULT_ALIGNMENT ? alignment : DEFAULT_ALIGNMENT);
		}

		void* VKAPI_PTR reallocate_callback(void*, void* original, size_t size, size_t alignment,
		    VkSystemAllocationScope scope)
		{
			if (original == nullptr)
			{
				return size == 0 ? nullptr : allocate_callback(nullptr, size, alignment, scope);
			}

			if (size == 0)
			{
				memory::free(original);
				return nullptr;
			}

			void* grown = allocate_callback(nullptr, size, alignment, scope);
			const size_t old = memory::usable_size(original);
			std::memcpy(grown, original, old < size ? old : size);
			memory::free(original);
			return grown;
		}

		void VKAPI_PTR free_callback(void*, void* block)
		{
			memory::free(block);
		}

		VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
		    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
		    const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
		{
			const char* id = data->pMessageIdName != nullptr ? data->pMessageIdName : "?";

			if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
			{
				s_validation_errors.fetch_add(1, std::memory_order_relaxed);
				EMBER_ERROR("vulkan[{}]: {}", id, data->pMessage);

				// Inside the offending call, so the stack shows the cause.
				if (s_break_on_validation_error)
				{
					EMBER_DEBUG_BREAK();
				}
			}
			else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0)
			{
				s_validation_warnings.fetch_add(1, std::memory_order_relaxed);
				EMBER_WARN("vulkan[{}]: {}", id, data->pMessage);
			}
			else
			{
				EMBER_TRACE("vulkan[{}]: {}", id, data->pMessage);
			}

			// Never abort the call: the layer's job is to report.
			return VK_FALSE;
		}

		// Vulkan's count then fetch, into a vector under Graphics. Boot memory, freed at the end of
		// the step that asked.
		template <typename T, typename F>
		Vector<T> enumerate(F fetch)
		{
			Vector<T> out{memory::Tag::Graphics};
			u32 count = 0;

			if (fetch(&count, nullptr) != VK_SUCCESS || count == 0)
			{
				return out;
			}

			out.resize(count);
			if (fetch(&count, out.data()) != VK_SUCCESS)
			{
				out.clear();
			}

			return out;
		}

		bool has_layer(Span<const VkLayerProperties> layers, const char* name)
		{
			for (const VkLayerProperties& layer : layers)
			{
				if (StringView(name) == layer.layerName)
					return true;
			}

			return false;
		}

		bool has_extension(Span<const VkExtensionProperties> extensions, const char* name)
		{
			for (const VkExtensionProperties& extension : extensions)
			{
				if (StringView(name) == extension.extensionName)
					return true;
			}

			return false;
		}

		// A required feature and its name, so an adapter that lacks one is refused by name.
		template <typename S>
		struct FeatureRef
		{
			VkBool32 S::* flag;
			const char* name;
		};

#define EMBER_FEATURE(S, member)                                                                   \
	FeatureRef<S>                                                                                  \
	{                                                                                              \
		&S::member, #member                                                                        \
	}

		// The feature set the engine requires, one table per chain struct. The same tables check
		// an adapter and enable the device, so the two can never drift.
		constexpr FeatureRef<VkPhysicalDeviceFeatures> REQUIRED_10[] = {
		    EMBER_FEATURE(VkPhysicalDeviceFeatures, samplerAnisotropy),
		    EMBER_FEATURE(VkPhysicalDeviceFeatures, depthClamp),
		    EMBER_FEATURE(VkPhysicalDeviceFeatures, depthBiasClamp),
		    EMBER_FEATURE(VkPhysicalDeviceFeatures, independentBlend),
		    EMBER_FEATURE(VkPhysicalDeviceFeatures, imageCubeArray),
		    EMBER_FEATURE(VkPhysicalDeviceFeatures, fragmentStoresAndAtomics),
		    EMBER_FEATURE(VkPhysicalDeviceFeatures, fullDrawIndexUint32),
		    EMBER_FEATURE(VkPhysicalDeviceFeatures, drawIndirectFirstInstance),
		    EMBER_FEATURE(VkPhysicalDeviceFeatures, shaderStorageImageExtendedFormats),
		    EMBER_FEATURE(VkPhysicalDeviceFeatures, multiDrawIndirect),
		    EMBER_FEATURE(VkPhysicalDeviceFeatures, textureCompressionBC),
		    EMBER_FEATURE(VkPhysicalDeviceFeatures, shaderStorageImageReadWithoutFormat),
		    EMBER_FEATURE(VkPhysicalDeviceFeatures, shaderStorageImageWriteWithoutFormat),
		};

		constexpr FeatureRef<VkPhysicalDeviceVulkan11Features> REQUIRED_11[] = {
		    EMBER_FEATURE(VkPhysicalDeviceVulkan11Features, shaderDrawParameters),
		};

		// The timelines, host query reset and the bindless heap.
		constexpr FeatureRef<VkPhysicalDeviceVulkan12Features> REQUIRED_12[] = {
		    EMBER_FEATURE(VkPhysicalDeviceVulkan12Features, timelineSemaphore),
		    EMBER_FEATURE(VkPhysicalDeviceVulkan12Features, hostQueryReset),
		    EMBER_FEATURE(VkPhysicalDeviceVulkan12Features, descriptorIndexing),
		    EMBER_FEATURE(VkPhysicalDeviceVulkan12Features, runtimeDescriptorArray),
		    EMBER_FEATURE(VkPhysicalDeviceVulkan12Features, descriptorBindingPartiallyBound),
		    EMBER_FEATURE(VkPhysicalDeviceVulkan12Features,
		        descriptorBindingVariableDescriptorCount),
		    EMBER_FEATURE(VkPhysicalDeviceVulkan12Features,
		        descriptorBindingSampledImageUpdateAfterBind),
		    EMBER_FEATURE(VkPhysicalDeviceVulkan12Features,
		        descriptorBindingStorageImageUpdateAfterBind),
		    EMBER_FEATURE(VkPhysicalDeviceVulkan12Features,
		        descriptorBindingStorageBufferUpdateAfterBind),
		    EMBER_FEATURE(VkPhysicalDeviceVulkan12Features,
		        descriptorBindingUpdateUnusedWhilePending),
		    EMBER_FEATURE(VkPhysicalDeviceVulkan12Features,
		        shaderSampledImageArrayNonUniformIndexing),
		    EMBER_FEATURE(VkPhysicalDeviceVulkan12Features,
		        shaderStorageBufferArrayNonUniformIndexing),
		    EMBER_FEATURE(VkPhysicalDeviceVulkan12Features,
		        shaderStorageImageArrayNonUniformIndexing),
		    EMBER_FEATURE(VkPhysicalDeviceVulkan12Features, scalarBlockLayout),
		};

		// Dynamic rendering and synchronization2 are how every pass and barrier is spelled. Shaders
		// lower discard to demote, which keeps a discarded lane alive as a helper so its quad's
		// derivatives stay defined along cutout edges.
		constexpr FeatureRef<VkPhysicalDeviceVulkan13Features> REQUIRED_13[] = {
		    EMBER_FEATURE(VkPhysicalDeviceVulkan13Features, dynamicRendering),
		    EMBER_FEATURE(VkPhysicalDeviceVulkan13Features, synchronization2),
		    EMBER_FEATURE(VkPhysicalDeviceVulkan13Features, maintenance4),
		    EMBER_FEATURE(VkPhysicalDeviceVulkan13Features, shaderDemoteToHelperInvocation),
		};

#undef EMBER_FEATURE

		// The chain of feature structs an adapter is asked about and a device is made with. It
		// points into itself, so it is never copied.
		struct Features
		{
			VkPhysicalDeviceFeatures2 base{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
			VkPhysicalDeviceVulkan11Features vulkan11{
			    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
			VkPhysicalDeviceVulkan12Features vulkan12{
			    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
			VkPhysicalDeviceVulkan13Features vulkan13{
			    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};

			Features()
			{
				base.pNext = &vulkan11;
				vulkan11.pNext = &vulkan12;
				vulkan12.pNext = &vulkan13;
			}

			Features(const Features&) = delete;
			Features& operator=(const Features&) = delete;
		};

		// Scans the whole table, so an adapter under the bar logs its complete gap list at once.
		template <typename S, size_t N>
		bool check(const S& available, const FeatureRef<S> (&required)[N], const char* adapter)
		{
			bool ok = true;
			for (const FeatureRef<S>& feature : required)
			{
				if (available.*feature.flag != VK_TRUE)
				{
					EMBER_INFO("vulkan: skipping {}: no {}", adapter, feature.name);
					ok = false;
				}
			}

			return ok;
		}

		template <typename S, size_t N>
		void enable(S& enabled, const FeatureRef<S> (&required)[N])
		{
			for (const FeatureRef<S>& feature : required)
			{
				enabled.*feature.flag = VK_TRUE;
			}
		}

		// What adapter selection learns and the rest of boot consumes. Dies with boot().
		struct AdapterInfo
		{
			VkPhysicalDevice handle = VK_NULL_HANDLE;
			VkPhysicalDeviceProperties properties = {};
			VkPhysicalDeviceMemoryProperties memory = {};

			u32 graphics_family = VK_QUEUE_FAMILY_IGNORED;
			u32 compute_family = VK_QUEUE_FAMILY_IGNORED;
			u32 transfer_family = VK_QUEUE_FAMILY_IGNORED;

			bool memory_budget = false;
			bool wireframe = false;
			bool indirect_count = false;
			bool sampler_minmax = false;
			u32 subgroup_size = 0;

			char driver[VK_MAX_DRIVER_NAME_SIZE + VK_MAX_DRIVER_INFO_SIZE + 1] = {};
		};

		AdapterKind to_adapter_kind(VkPhysicalDeviceType type)
		{
			switch (type)
			{
				case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
					return AdapterKind::Integrated;
				case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
					return AdapterKind::Discrete;
				case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
					return AdapterKind::Virtual;
				case VK_PHYSICAL_DEVICE_TYPE_CPU:
					return AdapterKind::Cpu;
				default:
					return AdapterKind::Other;
			}
		}

		// Higher wins, strictly, so enumeration order decides ties.
		int adapter_score(AdapterKind kind, AdapterPreference preference)
		{
			if (preference == AdapterPreference::Any)
			{
				return 1;
			}

			const bool integrated_first = preference == AdapterPreference::Integrated;
			switch (kind)
			{
				case AdapterKind::Discrete:
					return integrated_first ? 3 : 4;
				case AdapterKind::Integrated:
					return integrated_first ? 4 : 3;
				case AdapterKind::Virtual:
					return 2;
				case AdapterKind::Cpu:
					return 1;
				case AdapterKind::Other:
				case AdapterKind::Count:
					break;
			}

			return 0;
		}

		u32 find_family(Span<const VkQueueFamilyProperties> families, VkQueueFlags required,
		    VkQueueFlags forbidden)
		{
			for (u32 i = 0; i < families.size(); ++i)
			{
				const VkQueueFlags flags = families[i].queueFlags;
				if ((flags & required) == required && (flags & forbidden) == 0)
				{
					return i;
				}
			}

			return VK_QUEUE_FAMILY_IGNORED;
		}

		// The loader, the layers and extensions, the instance, volk's instance table and the debug
		// messenger.
		bool create_instance(vk::Backend& b, const GpuDef& def)
		{
			vk::Context& ctx = b.ctx;

			if (def.headless)
			{
				if (const VkResult result = volkInitialize(); result != VK_SUCCESS)
				{
					EMBER_ERROR("vulkan: no Vulkan loader on this system ({})",
					    vk::result_name(result));
					return false;
				}
			}
			else
			{
#if defined(EMBER_HAS_PLATFORM)
				// The platform owns the library when there is a window system: surface creation
				// shares its loader.
				const platform::vulkan::ProcAddress proc = platform::vulkan::load();
				if (proc == nullptr)
				{
					EMBER_ERROR("vulkan: the platform has no Vulkan loader");
					return false;
				}

				volkInitializeCustom((PFN_vkGetInstanceProcAddr)proc);
#else
				EMBER_ERROR("vulkan: this build has no window system; set GpuDef::headless");
				return false;
#endif
			}

			const u32 loader_version = volkGetInstanceVersion();
			if (loader_version < vk::API_VERSION)
			{
				EMBER_ERROR("vulkan: the loader supports {}.{}; Vulkan 1.3 is required",
				    VK_API_VERSION_MAJOR(loader_version), VK_API_VERSION_MINOR(loader_version));
				return false;
			}

			const Vector<VkLayerProperties> layers =
			    enumerate<VkLayerProperties>([](u32* count, VkLayerProperties* out)
			        { return vkEnumerateInstanceLayerProperties(count, out); });

			Vector<VkExtensionProperties> extensions =
			    enumerate<VkExtensionProperties>([](u32* count, VkExtensionProperties* out)
			        { return vkEnumerateInstanceExtensionProperties(nullptr, count, out); });

			const char* enabled_layers[1];
			const char* enabled_extensions[MAX_ENABLED_NAMES];
			u32 layer_count = 0;
			u32 extension_count = 0;
			bool validation = false;

			if (def.validation)
			{
				if (has_layer(layers, VALIDATION_LAYER))
				{
					enabled_layers[layer_count++] = VALIDATION_LAYER;
					validation = true;

					// The layer brings extensions of its own, VK_EXT_validation_features among
					// them.
					const Vector<VkExtensionProperties> own = enumerate<VkExtensionProperties>(
					    [](u32* count, VkExtensionProperties* out)
					    {
						    return vkEnumerateInstanceExtensionProperties(VALIDATION_LAYER, count,
						        out);
					    });
					extensions.insert(extensions.end(), own.begin(), own.end());

					EMBER_INFO("vulkan: validation on{}",
					    def.sync_validation ? ", synchronization too" : "");
				}
				else
				{
					EMBER_WARN("vulkan: {} is not installed; running without validation",
					    VALIDATION_LAYER);
				}
			}

			// Debug utils stays on in every build that has it: names and labels in a capture cost
			// nothing measurable.
			if (has_extension(extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
			{
				enabled_extensions[extension_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
				ctx.debug_utils = true;
			}

#if defined(EMBER_HAS_PLATFORM)
			if (!def.headless)
			{
				const Span<const char* const> wsi = platform::vulkan::instance_extensions();
				if (wsi.empty())
				{
					EMBER_ERROR("vulkan: the platform reports no window system support for Vulkan");
					return false;
				}

				for (const char* name : wsi)
				{
					if (!has_extension(extensions, name) || extension_count == MAX_ENABLED_NAMES)
					{
						EMBER_ERROR("vulkan: the window system needs {}, which the loader lacks",
						    name);
						return false;
					}

					enabled_extensions[extension_count++] = name;
				}
			}
#endif

			// Handing the messenger to vkCreateInstance validates instance creation and destruction
			// themselves; the same struct makes the persistent messenger below.
			const void* next = nullptr;

			VkDebugUtilsMessengerCreateInfoEXT messenger_info{
			    .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
			    .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
			                       VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
			    .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
			                   VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
			    .pfnUserCallback = debug_callback,
			};

			if (ctx.debug_utils)
			{
				messenger_info.pNext = next;
				next = &messenger_info;
			}

			const VkValidationFeatureEnableEXT sync_feature[] = {
			    VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};

			VkValidationFeaturesEXT validation_features{
			    .sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
			    .enabledValidationFeatureCount = 1,
			    .pEnabledValidationFeatures = sync_feature,
			};

			if (validation && def.sync_validation)
			{
				if (has_extension(extensions, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME))
				{
					enabled_extensions[extension_count++] =
					    VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME;
					validation_features.pNext = next;
					next = &validation_features;
				}
				else
				{
					EMBER_WARN(
					    "vulkan: synchronization validation was asked for, but {} is missing",
					    VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
				}
			}

			const VkApplicationInfo app_info{
			    .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
			    .pApplicationName = def.app_name,
			    .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
			    .pEngineName = "Ember",
			    .engineVersion = VK_MAKE_VERSION(0, 1, 0),
			    .apiVersion = vk::API_VERSION,
			};

			const VkInstanceCreateInfo info{
			    .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
			    .pNext = next,
			    .pApplicationInfo = &app_info,
			    .enabledLayerCount = layer_count,
			    .ppEnabledLayerNames = enabled_layers,
			    .enabledExtensionCount = extension_count,
			    .ppEnabledExtensionNames = enabled_extensions,
			};

			if (const VkResult result = vkCreateInstance(&info, &ctx.callbacks, &ctx.instance);
			    result != VK_SUCCESS)
			{
				EMBER_ERROR("vulkan: vkCreateInstance failed: {}", vk::result_name(result));
				ctx.instance = VK_NULL_HANDLE;
				return false;
			}

			// Instance entry points now; device ones load per device, skipping the trampoline.
			volkLoadInstanceOnly(ctx.instance);

			if (ctx.debug_utils)
			{
				messenger_info.pNext = nullptr;
				if (vkCreateDebugUtilsMessengerEXT(ctx.instance, &messenger_info, &ctx.callbacks,
				        &ctx.messenger) != VK_SUCCESS)
				{
					EMBER_WARN("vulkan: no debug messenger; continuing without one");
					ctx.messenger = VK_NULL_HANDLE;
				}
			}

			return true;
		}

		// Rejects an adapter that cannot run the engine's contract; fills [out] for one that can.
		bool query_adapter(const vk::Backend& b, VkPhysicalDevice handle, AdapterInfo& out)
		{
			out.handle = handle;
			vkGetPhysicalDeviceProperties(handle, &out.properties);
			vkGetPhysicalDeviceMemoryProperties(handle, &out.memory);

			const char* name = out.properties.deviceName;

			if (out.properties.apiVersion < vk::API_VERSION)
			{
				EMBER_INFO("vulkan: skipping {}: Vulkan {}.{} is below 1.3", name,
				    VK_API_VERSION_MAJOR(out.properties.apiVersion),
				    VK_API_VERSION_MINOR(out.properties.apiVersion));
				return false;
			}

			const Vector<VkExtensionProperties> extensions =
			    enumerate<VkExtensionProperties>([handle](u32* count, VkExtensionProperties* data)
			        { return vkEnumerateDeviceExtensionProperties(handle, nullptr, count, data); });

			if (!b.ctx.headless && !has_extension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
			{
				EMBER_INFO("vulkan: skipping {}: no {}", name, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
				return false;
			}

			out.memory_budget = has_extension(extensions, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);

			Features available;
			vkGetPhysicalDeviceFeatures2(handle, &available.base);

			bool ok = check(available.base.features, REQUIRED_10, name);
			ok &= check(available.vulkan11, REQUIRED_11, name);
			ok &= check(available.vulkan12, REQUIRED_12, name);
			ok &= check(available.vulkan13, REQUIRED_13, name);
			if (!ok)
			{
				return false;
			}

			out.wireframe = available.base.features.fillModeNonSolid == VK_TRUE;
			out.indirect_count = available.vulkan12.drawIndirectCount == VK_TRUE;
			out.sampler_minmax = available.vulkan12.samplerFilterMinmax == VK_TRUE;

			// The main family does graphics and compute, universal on desktop, and presents when
			// there is a window system: a separate present family would buy ownership transfers
			// every frame for nothing.
			const Vector<VkQueueFamilyProperties> families = enumerate<VkQueueFamilyProperties>(
			    [handle](u32* count, VkQueueFamilyProperties* data)
			    {
				    vkGetPhysicalDeviceQueueFamilyProperties(handle, count, data);
				    return VK_SUCCESS;
			    });

			for (u32 i = 0; i < families.size(); ++i)
			{
				constexpr VkQueueFlags GRAPHICS_COMPUTE =
				    VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
				if ((families[i].queueFlags & GRAPHICS_COMPUTE) != GRAPHICS_COMPUTE)
				{
					continue;
				}

#if defined(EMBER_HAS_PLATFORM)
				if (!b.ctx.headless &&
				    !platform::vulkan::presentation_supported(b.ctx.instance, handle, i))
				{
					continue;
				}
#endif

				out.graphics_family = i;
				break;
			}

			if (out.graphics_family == VK_QUEUE_FAMILY_IGNORED)
			{
				EMBER_INFO("vulkan: skipping {}: no graphics and compute{} queue family", name,
				    b.ctx.headless ? "" : " and present");
				return false;
			}

			// Families of their own for async compute and copies, when the adapter has them.
			out.compute_family = find_family(families, VK_QUEUE_COMPUTE_BIT, VK_QUEUE_GRAPHICS_BIT);
			out.transfer_family = find_family(families, VK_QUEUE_TRANSFER_BIT,
			    VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT);

			VkPhysicalDeviceDriverProperties driver{
			    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
			VkPhysicalDeviceVulkan11Properties properties11{
			    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES,
			    .pNext = &driver};
			VkPhysicalDeviceVulkan12Properties properties12{
			    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES,
			    .pNext = &properties11};
			VkPhysicalDeviceProperties2 properties2{
			    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
			    .pNext = &properties12};
			vkGetPhysicalDeviceProperties2(handle, &properties2);

			out.subgroup_size = properties11.subgroupSize;
			std::snprintf(out.driver, sizeof(out.driver), "%s %s", driver.driverName,
			    driver.driverInfo);

			// The engine is bindless, so a heap that cannot hold the pools is an adapter that
			// cannot run it. Refused here, where selection can still fall through.
			const GpuLimits& limits = b.def.limits;
			const auto fits = [name](u32 available, u32 needed, const char* what)
			{
				if (available >= needed)
				{
					return true;
				}

				EMBER_INFO("vulkan: skipping {}: update after bind {} {} < {}", name, what,
				    available, needed);
				return false;
			};

			bool heap_ok = fits(properties12.maxDescriptorSetUpdateAfterBindSampledImages,
			    limits.max_textures * SAMPLED_ARRAY_COUNT, "sampled images");
			heap_ok &= fits(properties12.maxDescriptorSetUpdateAfterBindStorageImages,
			    limits.max_textures * STORAGE_ARRAY_COUNT, "storage images");
			heap_ok &= fits(properties12.maxDescriptorSetUpdateAfterBindStorageBuffers,
			    limits.max_buffers, "storage buffers");
			heap_ok &= fits(properties12.maxDescriptorSetUpdateAfterBindSamplers,
			    limits.max_samplers, "samplers");
			heap_ok &= fits(properties12.maxPerStageUpdateAfterBindResources,
			    limits.max_textures * (SAMPLED_ARRAY_COUNT + STORAGE_ARRAY_COUNT) +
			        limits.max_buffers,
			    "per stage resources");

			return heap_ok;
		}

		// Filter then score: every adapter that can run the contract competes, and the best score
		// wins, with enumeration order deciding ties.
		bool select_adapter(const vk::Backend& b, const GpuDef& def, AdapterInfo& out)
		{
			const Vector<VkPhysicalDevice> adapters =
			    enumerate<VkPhysicalDevice>([&b](u32* count, VkPhysicalDevice* data)
			        { return vkEnumeratePhysicalDevices(b.ctx.instance, count, data); });

			if (adapters.empty())
			{
				EMBER_ERROR("vulkan: no physical devices");
				return false;
			}

			int best = -1;
			for (const VkPhysicalDevice handle : adapters)
			{
				AdapterInfo candidate;
				if (!query_adapter(b, handle, candidate))
				{
					continue;
				}

				const int score =
				    adapter_score(to_adapter_kind(candidate.properties.deviceType), def.adapter);
				if (score > best)
				{
					best = score;
					out = candidate;
				}
			}

			if (best < 0)
			{
				EMBER_ERROR(
				    "vulkan: no adapter has the Vulkan 1.3 feature set the engine requires");
				return false;
			}

			return true;
		}

		// The device with the required features and the optional ones the adapter has, one queue
		// per distinct family, and volk's device table.
		bool create_device(vk::Backend& b, const AdapterInfo& adapter)
		{
			vk::Context& ctx = b.ctx;

			const f32 priority = 1.0f;
			VkDeviceQueueCreateInfo queue_infos[vk::QUEUE_COUNT] = {};
			u32 queue_count = 0;

			const auto add_family = [&](u32 family)
			{
				if (family == VK_QUEUE_FAMILY_IGNORED)
				{
					return;
				}

				for (u32 i = 0; i < queue_count; ++i)
				{
					if (queue_infos[i].queueFamilyIndex == family)
					{
						return;
					}
				}

				queue_infos[queue_count++] = {
				    .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
				    .queueFamilyIndex = family,
				    .queueCount = 1,
				    .pQueuePriorities = &priority,
				};
			};

			add_family(adapter.graphics_family);
			add_family(adapter.compute_family);
			add_family(adapter.transfer_family);

			Features enabled;
			enable(enabled.base.features, REQUIRED_10);
			enable(enabled.vulkan11, REQUIRED_11);
			enable(enabled.vulkan12, REQUIRED_12);
			enable(enabled.vulkan13, REQUIRED_13);
			enabled.base.features.fillModeNonSolid = adapter.wireframe ? VK_TRUE : VK_FALSE;
			enabled.vulkan12.drawIndirectCount = adapter.indirect_count ? VK_TRUE : VK_FALSE;
			enabled.vulkan12.samplerFilterMinmax = adapter.sampler_minmax ? VK_TRUE : VK_FALSE;

			const char* extensions[2];
			u32 extension_count = 0;
			if (!ctx.headless)
			{
				extensions[extension_count++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
			}
			if (adapter.memory_budget)
			{
				extensions[extension_count++] = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
			}

			const VkDeviceCreateInfo info{
			    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
			    .pNext = &enabled.base,
			    .queueCreateInfoCount = queue_count,
			    .pQueueCreateInfos = queue_infos,
			    .enabledExtensionCount = extension_count,
			    .ppEnabledExtensionNames = extensions,
			};

			if (const VkResult result =
			        vkCreateDevice(adapter.handle, &info, &ctx.callbacks, &ctx.device);
			    result != VK_SUCCESS)
			{
				EMBER_ERROR("vulkan: vkCreateDevice failed: {}", vk::result_name(result));
				ctx.device = VK_NULL_HANDLE;
				return false;
			}

			volkLoadDevice(ctx.device);

			// One entry per distinct family; a Queue without a family of its own maps onto the
			// graphics entry, where its submissions run in order with everything else.
			const u32 families[vk::QUEUE_COUNT] = {adapter.graphics_family, adapter.compute_family,
			    adapter.transfer_family};

			ctx.queue_count = 0;
			for (u32 q = 0; q < vk::QUEUE_COUNT; ++q)
			{
				const u32 family = families[q];
				if (family == VK_QUEUE_FAMILY_IGNORED)
				{
					ctx.queue_index[q] = 0;
					continue;
				}

				u32 index = ctx.queue_count;
				for (u32 i = 0; i < ctx.queue_count; ++i)
				{
					if (ctx.queues[i].family == family)
					{
						index = i;
					}
				}

				if (index == ctx.queue_count)
				{
					vk::QueueState& queue = ctx.queues[ctx.queue_count++];
					queue.family = family;
					vkGetDeviceQueue(ctx.device, family, 0, &queue.handle);
				}

				ctx.queue_index[q] = (u8)index;
			}

			vk::set_name(ctx, VK_OBJECT_TYPE_DEVICE, (u64)ctx.device, "ember.device");
			vk::set_name(ctx, VK_OBJECT_TYPE_QUEUE, (u64)ctx.queues[0].handle,
			    "ember.queue.graphics");
			return true;
		}

		bool create_timelines(vk::Backend& b)
		{
			const VkSemaphoreTypeCreateInfo type_info{
			    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
			    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
			    .initialValue = 0,
			};

			const VkSemaphoreCreateInfo info{
			    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
			    .pNext = &type_info,
			};

			for (u32 i = 0; i < b.ctx.queue_count; ++i)
			{
				if (const VkResult result = vkCreateSemaphore(b.ctx.device, &info, &b.ctx.callbacks,
				        &b.ctx.queues[i].timeline);
				    result != VK_SUCCESS)
				{
					EMBER_ERROR("vulkan: timeline creation failed: {}", vk::result_name(result));
					return false;
				}

				vk::set_name(b.ctx, VK_OBJECT_TYPE_SEMAPHORE, (u64)b.ctx.queues[i].timeline,
				    "ember.timeline");
			}

			return true;
		}

		// VMA over volk's tables, with our callbacks for its CPU side and the budget as a limit
		// on every device local heap.
		bool create_allocator(vk::Backend& b, const AdapterInfo& adapter, const GpuDef& def)
		{
			VmaVulkanFunctions functions = {};
			functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
			functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

			VkDeviceSize limits[VK_MAX_MEMORY_HEAPS];
			for (u32 i = 0; i < VK_MAX_MEMORY_HEAPS; ++i)
			{
				const bool local =
				    i < adapter.memory.memoryHeapCount &&
				    (adapter.memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
				limits[i] = local && def.device_memory_budget != 0 ? def.device_memory_budget
				                                                   : VK_WHOLE_SIZE;
			}

			// Assignment rather than designated init: VMA's member order is not stable across
			// versions.
			VmaAllocatorCreateInfo info = {};
			info.physicalDevice = b.ctx.adapter;
			info.device = b.ctx.device;
			info.instance = b.ctx.instance;
			info.pAllocationCallbacks = &b.ctx.callbacks;
			info.pVulkanFunctions = &functions;
			info.pHeapSizeLimit = limits;
			info.vulkanApiVersion = vk::API_VERSION;
			if (adapter.memory_budget)
			{
				info.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
			}

			if (const VkResult result = vmaCreateAllocator(&info, &b.ctx.allocator);
			    result != VK_SUCCESS)
			{
				EMBER_ERROR("vulkan: vmaCreateAllocator failed: {}", vk::result_name(result));
				b.ctx.allocator = VK_NULL_HANDLE;
				return false;
			}

			return true;
		}

		// A pure transform of what selection learned; the only writer of Caps.
		void fill_caps(vk::Context& ctx, const AdapterInfo& adapter)
		{
			Caps& caps = ctx.caps;
			const VkPhysicalDeviceLimits& limits = adapter.properties.limits;

			std::snprintf(caps.adapter_name, sizeof(caps.adapter_name), "%.*s",
			    (int)(sizeof(caps.adapter_name) - 1), adapter.properties.deviceName);
			caps.vendor_id = adapter.properties.vendorID;
			caps.device_id = adapter.properties.deviceID;
			caps.api_version = adapter.properties.apiVersion;
			caps.adapter_kind = to_adapter_kind(adapter.properties.deviceType);

			// Total device local memory, and whether a large device local, host visible type
			// exists: a 256 MB heap is the old BAR window, anything larger is resizable BAR, SAM
			// or unified memory.
			const VkPhysicalDeviceMemoryProperties& memory = adapter.memory;
			caps.device_memory_bytes = 0;
			for (u32 i = 0; i < memory.memoryHeapCount; ++i)
			{
				if ((memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0)
				{
					caps.device_memory_bytes += memory.memoryHeaps[i].size;
				}
			}

			for (u32 i = 0; i < memory.memoryTypeCount; ++i)
			{
				constexpr VkMemoryPropertyFlags LOCAL_VISIBLE =
				    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
				const VkMemoryType& type = memory.memoryTypes[i];
				if ((type.propertyFlags & LOCAL_VISIBLE) == LOCAL_VISIBLE &&
				    memory.memoryHeaps[type.heapIndex].size > 256_mb)
				{
					caps.host_visible_device_memory = true;
				}
			}

			caps.constant_offset_alignment = (u32)limits.minUniformBufferOffsetAlignment;
			caps.storage_offset_alignment = (u32)limits.minStorageBufferOffsetAlignment;
			caps.copy_row_pitch_alignment = (u32)limits.optimalBufferCopyRowPitchAlignment;
			caps.copy_offset_alignment = (u32)limits.optimalBufferCopyOffsetAlignment;

			caps.max_texture_2d = limits.maxImageDimension2D;
			caps.max_texture_3d = limits.maxImageDimension3D;
			caps.max_texture_layers = limits.maxImageArrayLayers;
			caps.max_color_attachments = limits.maxColorAttachments < MAX_COLOR_ATTACHMENTS
			                                 ? limits.maxColorAttachments
			                                 : MAX_COLOR_ATTACHMENTS;
			caps.max_indirect_draw_count = limits.maxDrawIndirectCount;
			caps.max_anisotropy = (u32)limits.maxSamplerAnisotropy;
			caps.subgroup_size = adapter.subgroup_size;

			caps.timestamps =
			    limits.timestampComputeAndGraphics == VK_TRUE && limits.timestampPeriod > 0.0f;
			caps.timestamp_period_ns = limits.timestampPeriod;

			caps.wireframe = adapter.wireframe;
			caps.indirect_count = adapter.indirect_count;
			caps.sampler_minmax = adapter.sampler_minmax;
			caps.async_compute = adapter.compute_family != VK_QUEUE_FAMILY_IGNORED;
			caps.async_transfer = adapter.transfer_family != VK_QUEUE_FAMILY_IGNORED;

			// Guaranteed by the spec's minimums; asserted so a broken driver is loud.
			EMBER_ASSERT(limits.maxPushConstantsSize >= PUSH_CONSTANT_BYTES);
			EMBER_ASSERT(limits.maxBoundDescriptorSets >= 2);
		}

		bool create_list(vk::Backend& b, CommandList& list, u32 slot, u32 index)
		{
			list = CommandList{.slot = slot, .index = index};

			const VkCommandPoolCreateInfo pool_info{
			    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			    .queueFamilyIndex = b.ctx.queues[0].family,
			};

			if (vkCreateCommandPool(b.ctx.device, &pool_info, &b.ctx.callbacks, &list.pool) !=
			    VK_SUCCESS)
			{
				EMBER_ERROR("vulkan: command pool creation failed");
				return false;
			}

			const VkCommandBufferAllocateInfo buffer_info{
			    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			    .commandPool = list.pool,
			    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			    .commandBufferCount = 1,
			};

			if (vkAllocateCommandBuffers(b.ctx.device, &buffer_info, &list.commands) != VK_SUCCESS)
			{
				EMBER_ERROR("vulkan: command buffer allocation failed");
				return false;
			}

			return true;
		}

		// One slot per frame in flight, each with its lists, their pools and its zone table, and
		// the timestamp pool sliced across them.
		bool create_slots(vk::Backend& b)
		{
			const u32 in_flight = b.ctx.in_flight;
			const u32 max_lists = b.ctx.max_lists;
			const u32 zone_capacity = (max_lists + 1) * vk::MAX_ZONES;

			b.slots = (vk::Slot*)memory::allocate(memory::Tag::Graphics,
			    sizeof(vk::Slot) * in_flight, alignof(vk::Slot));

			for (u32 s = 0; s < in_flight; ++s)
			{
				vk::Slot& slot = *::new ((void*)(b.slots + s)) vk::Slot();

				slot.lists = (CommandList*)memory::allocate(memory::Tag::Graphics,
				    sizeof(CommandList) * max_lists, alignof(CommandList));
				for (u32 i = 0; i < max_lists; ++i)
				{
					::new ((void*)(slot.lists + i)) CommandList();
				}

				slot.zones = (ZoneTiming*)memory::allocate_zeroed(memory::Tag::Graphics,
				    sizeof(ZoneTiming) * zone_capacity, alignof(ZoneTiming));

				for (u32 i = 0; i < max_lists; ++i)
				{
					if (!create_list(b, slot.lists[i], s, i))
					{
						return false;
					}
				}

				if (!create_list(b, slot.finish, s, max_lists))
				{
					return false;
				}
			}

			if (b.ctx.caps.timestamps)
			{
				const u32 count = in_flight * zone_capacity * 2;
				const VkQueryPoolCreateInfo info{
				    .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
				    .queryType = VK_QUERY_TYPE_TIMESTAMP,
				    .queryCount = count,
				};

				if (vkCreateQueryPool(b.ctx.device, &info, &b.ctx.callbacks, &b.timestamps) !=
				    VK_SUCCESS)
				{
					EMBER_WARN("vulkan: no timestamp pool; GPU zones go untimed");
					b.timestamps = VK_NULL_HANDLE;
				}
				else
				{
					vkResetQueryPool(b.ctx.device, b.timestamps, 0, count);
				}
			}

			return true;
		}

		void destroy_slots(vk::Backend& b)
		{
			if (b.slots == nullptr)
			{
				return;
			}

			for (u32 s = 0; s < b.ctx.in_flight; ++s)
			{
				vk::Slot& slot = b.slots[s];

				if (slot.lists != nullptr)
				{
					for (u32 i = 0; i < b.ctx.max_lists; ++i)
					{
						vkDestroyCommandPool(b.ctx.device, slot.lists[i].pool, &b.ctx.callbacks);
					}
					memory::free(slot.lists);
				}

				vkDestroyCommandPool(b.ctx.device, slot.finish.pool, &b.ctx.callbacks);
				memory::free(slot.zones);
				slot.~Slot();
			}

			memory::free(b.slots);
			b.slots = nullptr;

			vkDestroyQueryPool(b.ctx.device, b.timestamps, &b.ctx.callbacks);
			b.timestamps = VK_NULL_HANDLE;
		}

		u32 clamp_limit(u32 value, u32 low, u32 high, const char* what)
		{
			if (value < low || value > high)
			{
				const u32 clamped = value < low ? low : high;
				EMBER_WARN("gpu: GpuLimits::{} clamped from {} to {}", what, value, clamped);
				return clamped;
			}

			return value;
		}
	}

	namespace vk
	{
		Backend* backend()
		{
			return s_backend;
		}

		void note_result(Backend& b, VkResult result)
		{
			if (result == VK_ERROR_DEVICE_LOST && !b.lost.exchange(true, std::memory_order_acq_rel))
			{
				EMBER_ERROR("vulkan: the device was lost; nothing more will complete");
			}
		}

		void set_name(const Context& ctx, VkObjectType type, u64 handle, const char* name)
		{
			// Without the extension volk holds a trampoline into a table that never implemented it.
			if (ctx.device == VK_NULL_HANDLE || !ctx.debug_utils || name == nullptr)
			{
				return;
			}

			const VkDebugUtilsObjectNameInfoEXT info{
			    .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
			    .objectType = type,
			    .objectHandle = handle,
			    .pObjectName = name,
			};
			(void)vkSetDebugUtilsObjectNameEXT(ctx.device, &info);
		}

		bool boot(Backend& b, const GpuDef& def)
		{
			Context& ctx = b.ctx;

			ctx.callbacks = {
			    .pfnAllocation = allocate_callback,
			    .pfnReallocation = reallocate_callback,
			    .pfnFree = free_callback,
			};
			ctx.headless = def.headless;
			ctx.in_flight = frames::in_flight();
			s_break_on_validation_error = def.break_on_validation_error;

			if (ctx.in_flight == 0 || ctx.in_flight > MAX_IN_FLIGHT)
			{
				EMBER_ERROR("gpu: frames::in_flight() is {}; the device holds 1 to {}",
				    ctx.in_flight, MAX_IN_FLIGHT);
				return false;
			}

			if (def.app_name == nullptr || def.transient_bytes == 0 || def.staging_bytes == 0)
			{
				EMBER_ERROR("gpu: an invalid GpuDef");
				return false;
			}

			GpuLimits& limits = b.def.limits;
			limits.max_textures = clamp_limit(limits.max_textures, 1,
			    Pool<TextureHot, Texture>::MAX_CAPACITY, "max_textures");
			limits.max_buffers =
			    clamp_limit(limits.max_buffers, 1, Handle<Buffer>::MAX_INDEX + 1, "max_buffers");
			limits.max_samplers =
			    clamp_limit(limits.max_samplers, 1, MAX_BINDLESS_SAMPLERS, "max_samplers");
			limits.max_swapchains =
			    clamp_limit(limits.max_swapchains, 1, MAX_SWAPCHAINS, "max_swapchains");
			limits.max_command_lists =
			    clamp_limit(limits.max_command_lists, 1, 256, "max_command_lists");
			ctx.max_lists = limits.max_command_lists;
			ctx.max_swapchains = limits.max_swapchains;

			memory::Allocator* heap = memory::heap();
			b.textures.initialize(heap, memory::Tag::Graphics, limits.max_textures);
			b.texture_cold = (TextureCold*)memory::allocate(memory::Tag::Graphics,
			    sizeof(TextureCold) * limits.max_textures, alignof(TextureCold));
			for (u32 i = 0; i < limits.max_textures; ++i)
			{
				::new ((void*)(b.texture_cold + i)) TextureCold();
			}
			b.swapchains.initialize(heap, memory::Tag::Graphics, limits.max_swapchains);

			if (!create_instance(b, def))
			{
				return false;
			}

			AdapterInfo adapter;
			if (!select_adapter(b, def, adapter))
			{
				return false;
			}

			ctx.adapter = adapter.handle;
			if (!create_device(b, adapter))
			{
				return false;
			}

			fill_caps(ctx, adapter);
			ctx.all_shader_stages = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
			                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
			                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

			if (!create_timelines(b) || !create_allocator(b, adapter, def) || !create_slots(b))
			{
				return false;
			}

			constexpr const char* KINDS[] = {"other", "integrated", "discrete", "virtual", "cpu"};
			EMBER_INFO("vulkan: {} ({}) | {} | Vulkan {}.{}.{} | {} MB{} | {} queue{}",
			    ctx.caps.adapter_name, KINDS[(u32)ctx.caps.adapter_kind], adapter.driver,
			    VK_API_VERSION_MAJOR(ctx.caps.api_version),
			    VK_API_VERSION_MINOR(ctx.caps.api_version),
			    VK_API_VERSION_PATCH(ctx.caps.api_version), ctx.caps.device_memory_bytes / 1_mb,
			    ctx.caps.host_visible_device_memory ? " (resizable BAR)" : "", ctx.queue_count,
			    ctx.queue_count == 1 ? "" : "s");

			return true;
		}

		void teardown(Backend& b)
		{
			Context& ctx = b.ctx;

			if (ctx.device != VK_NULL_HANDLE)
			{
				(void)vkDeviceWaitIdle(ctx.device);

				// Swapchains through their own path, so backbuffers go with them.
				SwapchainHandle swapchains[MAX_SWAPCHAINS];
				u32 swapchain_count = 0;
				for (auto it = b.swapchains.begin(); it != b.swapchains.end(); ++it)
				{
					swapchains[swapchain_count++] = it.handle();
				}
				for (u32 i = 0; i < swapchain_count; ++i)
				{
					destroy_swapchain(swapchains[i]);
				}

				// Textures the game forgot. Hidden mip entries die with their owners.
				u32 leaked = 0;
				for (auto it = b.textures.begin(); it != b.textures.end();)
				{
					const TextureHandle handle = it.handle();
					++it;

					if (b.texture_cold[handle.index()].parent.is_null())
					{
						++leaked;
						destroy(handle);
					}
				}

				if (leaked != 0)
				{
					EMBER_WARN("gpu: {} texture{} still alive at shutdown", leaked,
					    leaked == 1 ? "" : "s");
				}

				// Idle proves every value signalled, so everything in the graveyard may go.
				for (u32 i = 0; i < ctx.queue_count; ++i)
				{
					raise_completed(ctx.queues[i], ctx.queues[i].submitted);
				}
				drain(b, true);

				destroy_slots(b);
				for (u32 i = 0; i < ctx.queue_count; ++i)
				{
					vkDestroySemaphore(ctx.device, ctx.queues[i].timeline, &ctx.callbacks);
				}

				vmaDestroyAllocator(ctx.allocator);
				vkDestroyDevice(ctx.device, &ctx.callbacks);
			}

			if (ctx.instance != VK_NULL_HANDLE)
			{
				vkDestroyDebugUtilsMessengerEXT(ctx.instance, ctx.messenger, &ctx.callbacks);
				vkDestroyInstance(ctx.instance, &ctx.callbacks);
			}

			// Safe when volk never initialized: it nulls its pointers.
			volkFinalize();

#if defined(EMBER_HAS_PLATFORM)
			if (!ctx.headless)
			{
				platform::vulkan::unload();
			}
#endif

			b.swapchains.shutdown();
			b.textures.shutdown();
			memory::free(b.texture_cold);
			b.texture_cold = nullptr;
		}
	}

	bool initialize(const GpuDef& def)
	{
		EMBER_ASSERT(jobs::is_main());

		if (s_backend != nullptr)
		{
			EMBER_ERROR("gpu: initialize runs once");
			return false;
		}

		if (frames::in_flight() == 0)
		{
			EMBER_ERROR("gpu: frames::initialize() comes first");
			return false;
		}

		s_validation_errors.store(0, std::memory_order_relaxed);
		s_validation_warnings.store(0, std::memory_order_relaxed);

		s_backend = memory::new_object<vk::Backend>(memory::heap(), memory::Tag::Graphics);
		s_backend->def = def;

		if (!vk::boot(*s_backend, def))
		{
			shutdown();
			return false;
		}

		return true;
	}

	void shutdown()
	{
		if (s_backend == nullptr)
		{
			return;
		}

		EMBER_ASSERT(jobs::is_main());

		vk::Backend* dead = s_backend;
		vk::teardown(*dead);
		s_backend = nullptr;
		memory::delete_object(memory::heap(), dead);
	}

	u32 validation_errors()
	{
		return s_validation_errors.load(std::memory_order_relaxed);
	}

	u32 validation_warnings()
	{
		return s_validation_warnings.load(std::memory_order_relaxed);
	}
}
