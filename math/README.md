# Math

`Math` defines engine-owned value types used by World and Renderer contracts.

Public headers expose only GraphicsEngine types:

- `Vec2`, `Vec3`, `Vec4`;
- `Mat4`;
- `Quat`;
- `Transform`;
- camera projection descriptors.

GLM is an implementation detail. It is used through private conversion helpers
and must not appear in public `Math`, World or Renderer headers.

## Conventions

GraphicsEngine uses a right-handed world coordinate system:

- `+X` points right;
- `+Y` points up;
- forward view direction is `-Z`;
- `Cross(+X, +Y) = +Z`.

Matrices are stored in column-major order and multiplied with column vectors.
`ComposeTransform()` applies local scale, then rotation, then translation.

Clip-space depth follows Vulkan conventions: the near plane maps to `0`, the far
plane maps to `1`. The projection helpers expect radians.
