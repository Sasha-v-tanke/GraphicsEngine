#include <memory>
#include <vector>

#include <graphics/backend/backend.h>
#include <graphics/backend/factory.h>
#include <graphics/graphics.h>
#include <gtest/gtest.h>
#include <lib/common/error/exception.h>
#include <tests/common/test_error.h>

namespace {

using NTest::ExpectError;

struct FakeGraphicsState {
    NGraphics::GraphicsCapabilities Capabilities{
            .Presentation = true,
            .TimelineCompletion = true,
            .MaxFramesInFlight = 2,
    };
    int CreatedCount = 0;
    int DestroyedCount = 0;
    std::vector<NGraphics::FrameSubmission> Submissions;
    std::uint64_t NextCompletion = 1;
    std::uint64_t CompletedValue = 0;
};

thread_local FakeGraphicsState* g_fakeGraphicsState = nullptr;

class FakeGraphicsBackend final: public NGraphics::NBackend::IGraphicsBackend {
public:
    explicit FakeGraphicsBackend(FakeGraphicsState& state)
        : m_state(state) {
        ++m_state.CreatedCount;
    }

    ~FakeGraphicsBackend() override {
        ++m_state.DestroyedCount;
    }

    [[nodiscard]] const NGraphics::GraphicsCapabilities& GetCapabilities() const noexcept override {
        return m_state.Capabilities;
    }

    [[nodiscard]] std::uint64_t SubmitFrame(const NGraphics::FrameSubmission& submission) override {
        m_state.Submissions.push_back(submission);
        return m_state.NextCompletion++;
    }

    [[nodiscard]] bool IsCompleted(std::uint64_t completionValue) const override {
        return completionValue <= m_state.CompletedValue;
    }

private:
    FakeGraphicsState& m_state;
};

class GraphicsTest: public testing::Test {
protected:
    void SetUp() override {
        g_fakeGraphicsState = &m_state;
    }

    void TearDown() override {
        g_fakeGraphicsState = nullptr;
    }

    [[nodiscard]] static NGraphics::Graphics
    CreateGraphics(const NGraphics::RequiredGraphicsCapabilities& requiredCapabilities = {}) {
        return NGraphics::NBackend::CreateGraphicsForBackend(
                std::make_unique<FakeGraphicsBackend>(*g_fakeGraphicsState),
                requiredCapabilities);
    }

    FakeGraphicsState m_state;
};

TEST_F(GraphicsTest, CreatesBackendAndPublishesImmutableCapabilities) {
    {
        const NGraphics::Graphics graphics = CreateGraphics();

        EXPECT_EQ(m_state.CreatedCount, 1);
        EXPECT_EQ(m_state.DestroyedCount, 0);
        EXPECT_TRUE(graphics.GetCapabilities().Presentation);
        EXPECT_TRUE(graphics.GetCapabilities().TimelineCompletion);
        EXPECT_EQ(graphics.GetCapabilities().MaxFramesInFlight, 2U);
    }

    EXPECT_EQ(m_state.DestroyedCount, 1);
}

TEST_F(GraphicsTest, RejectsBackendThatDoesNotSatisfyRequiredCapabilities) {
    m_state.Capabilities.TimelineCompletion = false;

    NGraphics::RequiredGraphicsCapabilities requiredCapabilities;
    requiredCapabilities.TimelineCompletion = true;

    ExpectError(NCommon::EError::UNSUPPORTED,
                [&] { const NGraphics::Graphics graphics = CreateGraphics(requiredCapabilities); });

    EXPECT_EQ(m_state.CreatedCount, 1);
    EXPECT_EQ(m_state.DestroyedCount, 1);
}

TEST_F(GraphicsTest, SubmitsFrameAndReportsCompletionPoint) {
    NGraphics::Graphics graphics = CreateGraphics();

    const auto completion = graphics.SubmitFrame({
            .FrameIndex = 42,
            .RequiresPresentation = true,
    });

    ASSERT_TRUE(completion.IsValid());
    ASSERT_EQ(m_state.Submissions.size(), 1U);
    EXPECT_EQ(m_state.Submissions[0].FrameIndex, 42U);
    EXPECT_TRUE(m_state.Submissions[0].RequiresPresentation);
    EXPECT_FALSE(graphics.IsCompleted(completion));

    m_state.CompletedValue = completion.GetValue();

    EXPECT_TRUE(graphics.IsCompleted(completion));
}

TEST_F(GraphicsTest, RejectsPresentationSubmissionWhenBackendHasNoPresentationPath) {
    m_state.Capabilities.Presentation = false;

    NGraphics::RequiredGraphicsCapabilities requiredCapabilities;
    requiredCapabilities.Presentation = false;

    NGraphics::Graphics graphics = CreateGraphics(requiredCapabilities);

    ExpectError(NCommon::EError::UNSUPPORTED, [&] {
        (void)graphics.SubmitFrame({
                .FrameIndex = 1,
                .RequiresPresentation = true,
        });
    });
}

TEST_F(GraphicsTest, RejectsInvalidCompletionPoint) {
    const NGraphics::Graphics graphics = CreateGraphics();

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)graphics.IsCompleted(NGraphics::CompletionPoint{}); });
}

TEST_F(GraphicsTest, RejectsCompletionPointFromAnotherGraphics) {
    NGraphics::Graphics first = CreateGraphics();

    FakeGraphicsState otherState;
    g_fakeGraphicsState = &otherState;
    NGraphics::Graphics second = CreateGraphics();
    g_fakeGraphicsState = &m_state;

    const auto completion = first.SubmitFrame({
            .FrameIndex = 1,
            .RequiresPresentation = true,
    });

    ExpectError(NCommon::EError::INVALID_ARGUMENT, [&] { (void)second.IsCompleted(completion); });
}

} // namespace
