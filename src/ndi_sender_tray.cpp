#include "ndi_sender.h"
#include "tray_controller.h"

bool NdiSender::sendFrame(const std::uint8_t* data, int width, int height, int fps) {
    const std::uint8_t* frameData = data;
    int frameWidth = width;
    int frameHeight = height;

    if (trayImageHidden()) {
        const std::uint8_t* hiddenData = nullptr;
        int hiddenWidth = 0;
        int hiddenHeight = 0;
        if (trayHiddenFrame(hiddenData, hiddenWidth, hiddenHeight)) {
            frameData = hiddenData;
            frameWidth = hiddenWidth;
            frameHeight = hiddenHeight;
        }
    }

    return sendFrameRaw(frameData, frameWidth, frameHeight, fps);
}
