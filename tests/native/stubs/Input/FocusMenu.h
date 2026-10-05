#pragma once

// Engine menu boundary only; InputCaptureController is compiled unchanged.
namespace OSFUI
{
    class FocusMenu
    {
    public:
        static bool Register();
        static void Open();
        static void Close();
        static bool IsRegistered();
        static bool IsOpenInEngine();
        static void SetGamepadCapture(bool capture);
    };
}
