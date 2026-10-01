# Graphics

`Graphics` is the backend-independent GPU execution boundary. Public descriptors describe semantic GPU work and never
expose native backend handles.

## Graphics pipelines

`GraphicsPipelineDescriptor` is copied when a pipeline is created. The published pipeline state is immutable for the
lifetime of its `GraphicsPipelineHandle`; changing the caller's descriptor does not mutate an existing pipeline.

A `Shader` references a typed `ResourceHandle<NResources::ShaderArtifact>`, keeps its canonical resource identity,
and retains an immutable shared snapshot of the ready cooked artifact. Graphics does not define a second shader stage or
SPIR-V artifact model. Resource unload therefore stops future acquisition without invalidating a pipeline descriptor
that already retained its artifact snapshot.

The descriptor contains resource-backed shaders, vertex input, topology, raster/depth/blend state, attachment formats,
and sample count. Shader vector order does not affect pipeline identity; supported graphics stages are canonicalized by
stage for equality and hashing. A vertex shader is required for the current primitive path. A fragment shader is
required when color attachments are declared, while depth-only pipelines may omit it. Runtime shader compilation is
outside the Graphics runtime contract.

Pipeline compatibility is validated before backend creation. Invalid shader stages, backend-mapped enum values, vertex
layouts, attachment formats, blend counts, depth configuration, and sample counts fail with a structured GraphicsEngine
error.

The descriptor hash includes canonical shader resource identity, entry point, and the retained artifact contents
together with all fixed-function pipeline state. It does not depend on runtime resource slots. Republishing different
cooked shader contents therefore produces a different pipeline key even when the logical resource handle remains the
same.

## Backend boundary

Native shader modules and native pipelines belong to the backend. The public API exposes only
`GraphicsPipelineDescriptor` and `GraphicsPipelineHandle`. Vulkan conversion, `VkShaderModule`, `VkPipeline`, and
pipeline cache mechanics remain backend-local.

Pipeline destruction accepts an optional `CompletionPoint`, so a backend can defer physical destruction until all
submitted GPU users are complete.
