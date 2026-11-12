#include <GraphicsEngine/application/application.h>

// This target must fail compilation: the old callback is an intentional API break.
class LegacyApplication: public NApplication::Application {
    void OnDraw() override {
    }
};
