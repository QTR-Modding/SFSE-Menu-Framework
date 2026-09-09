#include "input/GamepadNavigation.h"
#include "ui/McpWindow.h"

#include <RE/B/BSInputEventUser.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <new>

namespace Pad = SFSEMenuFramework::GamepadNavigation;

namespace
{
    bool consumeBack{};
    int backRequests{};

    void Check(bool condition, const char* message)
    {
        if (!condition) {
            std::cerr << message << '\n';
            std::exit(1);
        }
    }
}

bool SFSEMenuFramework::McpWindow::ConsumeGamepadBack()
{
    ++backRequests;
    return consumeBack;
}

namespace
{
    struct Fixture
    {
        ImGuiContext* Context = ImGui::CreateContext();
        std::uint64_t Generation = 100;
        alignas(RE::ButtonEvent) std::byte EventStorage[sizeof(RE::ButtonEvent)]{};
        RE::ButtonEvent* Event = new (EventStorage) RE::ButtonEvent{};

        Fixture()
        {
            auto& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.DisplaySize = {800, 600};
            io.DeltaTime = 1.0F / 60.0F;
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
            io.Fonts->AddFontDefault();
            unsigned char* pixels{};
            int width{}, height{};
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
            Event->deviceType = RE::InputEvent::DeviceType::kGamepad;
            Event->eventType = RE::InputEvent::EventType::kButton;
            Reset();
        }

        ~Fixture()
        {
            ImGui::DestroyContext(Context);
            // No string is assigned or acquired. ButtonEvent's destructor calls
            // the game's string-pool relocation, unavailable in this headless test.
        }

        void Reset()
        {
            ++Generation;
            consumeBack = false;
            backRequests = 0;
            ImGui::GetIO().ClearEventsQueue();
            ImGui::GetIO().ClearInputKeys();
            Frame(false, false);
        }

        void Capture(std::int32_t id, float value, float held = 0.0F)
        {
            Event->idCode = id;
            Event->value = value;
            Event->heldDownSecs = held;
            Pad::CaptureNativeEvent(*Event, Generation, true);
        }

        void Back(float value, float held = 0.0F) { Capture(8192, value, held); }

        void Frame(bool expectedDown, bool expectedPress)
        {
            Pad::ApplyPending(Generation);
            if (!expectedDown) {
                for (const auto& event : Context->InputEventsQueue) {
                    Check(event.Type != ImGuiInputEventType_Key ||
                        event.Key.Key != ImGuiKey_GamepadFaceRight || !event.Key.Down,
                        "a handled B sequence must not queue an ImGui B press");
                }
            }
            ImGui::NewFrame();
            Check(ImGui::IsKeyDown(ImGuiKey_GamepadFaceRight) == expectedDown,
                "unexpected ImGui B down state");
            Check(ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) == expectedPress,
                "unexpected ImGui B press edge");
            ImGui::EndFrame();
        }
    };

    void TestConsumedSequence(Fixture& ui)
    {
        ui.Reset();
        consumeBack = true;
        ui.Back(1);
        ui.Frame(false, false);
        Check(backRequests == 1, "B initial press reaches page/window routing once");
        consumeBack = false;
        ui.Back(1, 0.1F);
        ui.Frame(false, false);
        ui.Back(1, 0.2F);
        ui.Frame(false, false);
        ui.Back(0, 0.3F);
        ui.Frame(false, false);
        Check(backRequests == 1, "held and release samples must not reroute Back");

        ui.Back(1);
        ui.Frame(true, true);
        Check(backRequests == 2, "the next independent B press reaches routing");
        ui.Back(1, 0.1F);
        ui.Frame(true, false);
        ui.Back(0, 0.2F);
        ui.Frame(false, false);

        consumeBack = true;
        ui.Back(1);
        ui.Back(1, 0.1F);
        ui.Back(0, 0.2F);
        ui.Frame(false, false);
        Check(backRequests == 3, "a complete queued B sequence routes only once");
    }

    void TestGenerationReset(Fixture& ui)
    {
        ui.Reset();
        consumeBack = true;
        ui.Back(1);
        ui.Frame(false, false);
        Check(backRequests == 1, "old generation consumed B");

        ++ui.Generation;
        consumeBack = false;
        ui.Back(1, 0.1F);
        ui.Frame(true, true);
        Check(backRequests == 1, "new-generation held input is not an initial Back action");
        ui.Back(0, 0.2F);
        ui.Frame(false, false);
    }

    void TestOverflowRecovery(Fixture& ui)
    {
        ui.Reset();
        consumeBack = true;
        ui.Back(1);
        ui.Frame(false, false);

        // Exceed the production queue's 512-event capacity without processing.
        for (int index = 0; index < 600; ++index) {
            ui.Capture(4096, index % 2 == 0 ? 1.0F : 0.0F);
        }
        ui.Frame(false, false);
        Check(backRequests == 1, "overflow does not manufacture a Back request");

        consumeBack = false;
        // Dropping queued samples must not resurrect the consumed held B.
        ui.Back(1, 0.1F);
        ui.Frame(false, false);
        ui.Back(0, 0.2F);
        ui.Frame(false, false);
        ui.Back(1);
        ui.Frame(true, true);
        Check(backRequests == 2, "ordinary B routing resumes after overflow");
        ui.Back(0, 0.1F);
        ui.Frame(false, false);
    }
}

int main()
{
    Fixture ui;
    TestConsumedSequence(ui);
    TestGenerationReset(ui);
    TestOverflowRecovery(ui);
    std::cout << "Native gamepad B sequence tests passed\n";
}
