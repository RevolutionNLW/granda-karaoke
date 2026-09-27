#include "ocr/PlatformTitleScreenOcr.h"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <Vision/Vision.h>

namespace {

// CD+G frames are only 300x216 with blocky glyphs; nearest-neighbour
// enlargement keeps the edges sharp for the recogniser.
constexpr int kScale = 4;

class VisionTitleScreenOcr final : public TitleScreenOcrEngine {
public:
    QString name() const override { return QStringLiteral("apple-vision-accurate-1"); }

    bool recognise(const std::vector<std::uint32_t>& argb, int width, int height,
                   QList<OcrLine>* lines, QString* error) override
    {
        if (width <= 0 || height <= 0 || argb.size() != std::size_t(width) * std::size_t(height)) {
            if (error)
                *error = QStringLiteral("Invalid frame size");
            return false;
        }
        const int scaledWidth = width * kScale;
        const int scaledHeight = height * kScale;
        std::vector<std::uint32_t> scaled(std::size_t(scaledWidth) * std::size_t(scaledHeight));
        for (int y = 0; y < scaledHeight; ++y) {
            const std::uint32_t* source = argb.data() + std::size_t(y / kScale) * std::size_t(width);
            std::uint32_t* target = scaled.data() + std::size_t(y) * std::size_t(scaledWidth);
            for (int x = 0; x < scaledWidth; ++x)
                target[x] = source[x / kScale] | 0xff000000u;
        }
        bool ok = false;
        @autoreleasepool {
            CGColorSpaceRef colourSpace = CGColorSpaceCreateDeviceRGB();
            // 0xAARRGGBB words in native (little-endian) order are BGRA bytes.
            CGContextRef context = CGBitmapContextCreate(
                scaled.data(), size_t(scaledWidth), size_t(scaledHeight), 8,
                size_t(scaledWidth) * 4, colourSpace,
                static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Little)
                    | static_cast<CGBitmapInfo>(kCGImageAlphaNoneSkipFirst));
            CGImageRef image = context ? CGBitmapContextCreateImage(context) : nullptr;
            if (image) {
                VNRecognizeTextRequest* request = [[VNRecognizeTextRequest alloc] init];
                request.recognitionLevel = VNRequestTextRecognitionLevelAccurate;
                request.recognitionLanguages = @[@"en-US"];
                request.usesLanguageCorrection = NO;
                VNImageRequestHandler* handler =
                    [[VNImageRequestHandler alloc] initWithCGImage:image options:@{}];
                NSError* failure = nil;
                if ([handler performRequests:@[request] error:&failure]) {
                    ok = true;
                    for (VNRecognizedTextObservation* observation in request.results) {
                        VNRecognizedText* best = [[observation topCandidates:1] firstObject];
                        if (!best)
                            continue;
                        const CGRect box = observation.boundingBox;  // bottom-left origin
                        OcrLine line;
                        line.text = QString::fromNSString(best.string);
                        line.confidence = best.confidence;
                        line.x = box.origin.x;
                        line.y = 1.0 - box.origin.y - box.size.height;
                        line.width = box.size.width;
                        line.height = box.size.height;
                        lines->append(line);
                    }
                } else if (error) {
                    *error = failure ? QString::fromNSString(failure.localizedDescription)
                                     : QStringLiteral("Text recognition failed");
                }
                CGImageRelease(image);
            } else if (error) {
                *error = QStringLiteral("Could not create an image for text recognition");
            }
            if (context)
                CGContextRelease(context);
            CGColorSpaceRelease(colourSpace);
        }
        return ok;
    }
};

} // namespace

std::unique_ptr<TitleScreenOcrEngine> createPlatformTitleScreenOcr()
{
    return std::make_unique<VisionTitleScreenOcr>();
}
