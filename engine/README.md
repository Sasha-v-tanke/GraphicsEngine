# Engine

## FrameScheduler

`FrameScheduler` управляет bounded lifetime одновременно активных кадров.

Количество slot'ов задаётся через:

```cpp
NEngine::EngineConfig{
    .MaxActiveFrames = 2,
};
```

