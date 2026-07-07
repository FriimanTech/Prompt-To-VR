\#\# VR Engine components

\#\#\# Build & toolchain  
\- \*\*C++20\*\* — Language standard for the native runtime (\`ptv\_runtime\`).  
\- \*\*CMake\*\* — Configures, links, and builds the engine, tools, samples, and per-project executables.  
\- \*\*MSVC / Visual Studio Build Tools\*\* — Windows compiler and linker (VS 2022/2026, toolset v145).  
\- \*\*vcpkg (manifest mode)\*\* — Dependency manager; \`engine/vcpkg.json\` pins all third-party libraries.

\#\#\# Graphics, XR & platform libraries  
\- \*\*Vulkan\*\* — Low-level GPU API for rendering.  
\- \*\*volk\*\* — Vulkan function loader/dispatch.  
\- \*\*Vulkan Memory Allocator (VMA)\*\* — GPU buffer/image allocation.  
\- \*\*OpenXR\*\* — VR session, head/hand tracking, controller input, per-eye swapchains (Quest Link).  
\- \*\*GLFW\*\* — Desktop mirror window (not the HMD display).  
\- \*\*GLM\*\* — Math types (\`Vec3\`, \`Quat\`, matrices) via \`ptv/math/\`.  
\- \*\*GLSL shaders\*\* — \`mesh.vert/frag\`, \`scene.vert/frag\` for the forward renderer.  
\- \*\*Dear ImGui\*\* — Debug overlay on the mirror window only.

\#\#\# Simulation & data libraries  
\- \*\*EnTT\*\* — Entity-component-system (ECS) registry.  
\- \*\*Jolt Physics\*\* — Rigid-body collision and simulation.  
\- \*\*tinygltf\*\* — Loads GLB meshes referenced by \`scene.json\`.  
\- \*\*nlohmann-json\*\* — Parses \`scene.json\` and save files.  
\- \*\*spdlog\*\* — Structured logging.

\#\#\# Hand-authored engine subsystems (\`engine/runtime\`)  
\- \*\*\`Application\`\*\* — Top-level owner; runs the main loop (XR frame, input, locomotion, grab/throw, mechanics, physics, render).  
\- \*\*\`Window\` (Platform)\*\* — GLFW mirror window for desktop preview.  
\- \*\*\`VulkanContext\`\*\* — Vulkan instance, device, queues, VMA allocator.  
\- \*\*\`XrRuntime\`\*\* — OpenXR instance/session, stage reference space, Touch controller action set, per-eye swapchains, frame loop.  
\- \*\*\`Renderer\`\*\* — Vulkan render pass, graphics pipeline, depth buffers, GLB mesh cache, per-eye drawing into XR swapchain images.  
\- \*\*\`GltfMeshLoad\`\*\* — Uploads GLB geometry/materials into GPU mesh slots.  
\- \*\*\`World\` (ECS)\*\* — EnTT registry with stable string IDs; components like \`CTransform\`, \`CMesh\`, \`CPhysics\`, \`CMechanicBinding\`.  
\- \*\*\`SceneLoader\` / \`SceneDoc\`\*\* — Reads compiled \`scene.json\` and populates the ECS (entities, meshes, physics, mechanic bindings, spawn, lights).  
\- \*\*\`PhysicsWorld\`\*\* — Builds Jolt bodies from ECS, steps simulation, syncs transforms back.  
\- \*\*\`MechanicRegistry\` \+ \`MechanicHost\`\*\* — Factory registry and per-frame host for gameplay modules.  
\- \*\*\`BuiltinMechanics\`\*\* — Built-in VR mechanics (grab, throw, trigger zones, etc.) registered at static init.  
\- \*\*\`DebugUI\`\*\* — ImGui diagnostics on the mirror window.  
\- \*\*\`SaveSystem\`\*\* — JSON world-state snapshots (overlays on top of scene load).

\#\#\# Extension & content boundaries  
\- \*\*\`scene.json\`\*\* — Deterministic scene contract the runtime loads (from the Python scene compiler in the orchestrator).  
\- \*\*\`ptv/api/\` interfaces\*\* — \`IMechanic\`, \`IBehaviour\`, \`IDialogueHook\` — the only API surface LLM-generated code may use.  
\- \*\*\`engine/generated/{mechanics,ai,dialogue}/\`\*\* — Per-project LLM-produced C++ that registers into \`MechanicRegistry\` (bounded by \`check\_generated\_bounds.py\`).  
\- \*\*\`engine/tools/\`\*\* — Build-time helpers (\`glb\_validate\`, generated-code bounds checker).

\#\#\# Full PromptVR stack (feeds the engine, not inside the runtime)  
\- \*\*Orchestrator (FastAPI)\*\* — Runs the prompt→artifact pipeline and invokes CMake build \+ exe launch.  
\- \*\*\`packages/schemas\`\*\* — Pydantic models (\`BuildSpec\`, \`SceneSpec\`, etc.) that become JSON artifacts.  
\- \*\*Provider clients\*\* — Anthropic, Google Imagen/Gemini, Tripo — produce images, GLBs, and codegen inputs upstream of the engine.

\---

\#\# How they chain together

At build time, \*\*CMake\*\* \+ \*\*vcpkg\*\* pull in \*\*Vulkan\*\*, \*\*OpenXR\*\*, \*\*EnTT\*\*, \*\*Jolt\*\*, \*\*GLFW\*\*, \*\*GLM\*\*, \*\*tinygltf\*\*, and the rest, then compile the static library \*\*\`ptv\_runtime\`\*\* and link it into an executable (e.g. \`ptv\_starter\_room.exe\` or a per-project \`ptv\_\<project\>.exe\`) together with any \*\*\`engine/generated/\`\*\* modules for that game. At launch, \*\*\`Application::run()\`\*\* initializes in a fixed order: \*\*\`Window\`\*\* creates the GLFW mirror; \*\*\`VulkanContext\`\*\* brings up the GPU; \*\*\`XrRuntime\`\*\* creates an OpenXR session bound to that Vulkan device via \`XR\_KHR\_vulkan\_enable2\`, establishes the \*\*STAGE\*\* reference space, and sets up Oculus Touch input (grip/aim poses, triggers, grips, thumbsticks); \*\*\`Renderer\`\*\* builds pipelines and mesh caches on top of Vulkan \+ VMA; then \*\*\`LoadSceneJson\`\*\* reads \*\*\`scene.json\`\*\* (path from argv, \`PTV\_SCENE\_JSON\`, or fallback) into a \*\*\`SceneDoc\`\*\*, and \*\*\`ApplySceneToWorld\`\*\* spawns \*\*EnTT\*\* entities with transforms, GLB mesh references, physics props, and mechanic bindings. \*\*\`PhysicsWorld::rebuild\_from\_world\`\*\* turns \`CPhysics\` components into \*\*Jolt\*\* rigid bodies; \*\*\`MechanicHost::attach\_bindings\`\*\* uses \*\*\`MechanicRegistry\`\*\* to instantiate built-in and generated \*\*\`IMechanic\`\*\* implementations from each entity’s \`CMechanicBinding\`. Each frame, \*\*\`XrRuntime\`\*\* polls events, waits on the next XR frame, locates head and hand views in STAGE space, and acquires per-eye swapchain images; \*\*\`Application\`\*\* applies VR locomotion (teleport / head-relative / controller-relative slide), grab/throw, and snap-turn through the player rig, then calls \*\*\`mechanics\_.update()\`\*\*, \*\*\`physics\_.step()\`\*\* (Jolt workers run internally), and \*\*\`physics\_.sync\_to\_world()\`\*\* to write simulated transforms back into the ECS. \*\*\`Renderer::draw\_frame\`\*\* walks visible entities, loads GLBs through \*\*tinygltf\*\* into mesh slots, and submits Vulkan draw calls into each eye’s OpenXR swapchain image using view/projection matrices from OpenXR; the same frame can be mirrored to the GLFW window, with optional \*\*Dear ImGui\*\* debug UI. \*\*\`SaveSystem\`\*\* can persist runtime state as JSON overlays without replacing the authoritative scene. Upstream of all of this, the \*\*orchestrator pipeline\*\* turns a user prompt into the artifacts the engine consumes: structured JSON plans → reference images → Tripo GLBs → deterministic scene compile → \*\*\`scene.json\`\*\* \+ generated C++ → \*\*CMake\*\* build → OpenXR launch on Quest Link — so the engine core stays stable hand-authored C++, while each prompt only changes data (\`scene.json\`, GLB assets) and bounded generated gameplay code, not the renderer, XR loop, or ECS core.  