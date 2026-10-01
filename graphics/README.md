# Graphics

`Graphics` is the backend-independent GPU execution boundary. Public descriptors describe semantic GPU work and never
expose native backend handles.

## Graphics pipelines

`GraphicsPipelineDescriptor` is copied when a pipeline is created. The published pipeline state is immutable for the
lifetime of its `GraphicsPipelineHandle`; changing the caller's descriptor does not mutate an existing pipeline.

The descriptor contains cooked shader artifacts, vertex input, topology, raster/depth/blend state, attachment formats,
and sample count. Runtime shader compilation is outside the Graphics runtime contract.

Pipeline compatibility is validated before backend creation. Invalid shader stages, vertex layouts, attachment formats,
blend counts, depth configuration, and sample counts fail with a structured GraphicsEngine error.

The descriptor hash is deterministic over all pipeline-defining state and is intended as the foundation for the
content-addressed `PipelineManager` cache.

## Backend boundary

Native shader modules and native pipelines belong to the backend. The public API exposes only
`GraphicsPipelineDescriptor` and `GraphicsPipelineHandle`. Vulkan conversion, `VkShaderModule`, `VkPipeline`, and
pipeline cache mechanics remain backend-local.

Pipeline destruction accepts an optional `CompletionPoint`, so a backend can defer physical destruction until all
submitted GPU users are complete.
