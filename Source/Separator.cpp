#include "Separator.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include "model.hpp"
#include <thread>
#include <mutex>
#include <cmath>

namespace Separator
{

juce::File modelsDir()
{
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
   #if JUCE_MAC
    base = base.getChildFile ("Application Support");
   #endif
    auto dir = base.getChildFile ("WXRST").getChildFile ("models");
    dir.createDirectory();
    return dir;
}

juce::File modelFile (bool six)
{
    return modelsDir().getChildFile (six ? "ggml-model-htdemucs-6s-f16.bin" : "ggml-model-htdemucs-4s-f16.bin");
}

bool modelAvailable (bool six)
{
    auto f = modelFile (six);
    return f.existsAsFile() && f.getSize() > 1000000;
}

juce::URL modelURL (bool six)
{
    return juce::URL ("https://huggingface.co/datasets/Retrobear/demucs.cpp/resolve/main/" + modelFile (six).getFileName());
}

juce::int64 modelSizeBytes (bool six) { return six ? 54855129 : 83994361; }

juce::AudioBuffer<float> resample (const juce::AudioBuffer<float>& in, double inRate, double outRate, int outChannels)
{
    const int inLen = in.getNumSamples();
    if (inLen == 0) return juce::AudioBuffer<float> (outChannels, 0);
    const double ratio = inRate / outRate;
    const int outLen = (int) std::ceil ((double) inLen / ratio);
    juce::AudioBuffer<float> out (outChannels, outLen);
    out.clear();
    for (int ch = 0; ch < outChannels; ++ch)
    {
        // mono -> stereo duplicates; stereo -> mono averages
        juce::AudioBuffer<float> src (1, inLen);
        if (in.getNumChannels() == 1 || (outChannels == 1 && in.getNumChannels() == 1))
            src.copyFrom (0, 0, in, 0, 0, inLen);
        else if (outChannels == 1)
        {
            src.copyFrom (0, 0, in, 0, 0, inLen);
            for (int c = 1; c < in.getNumChannels(); ++c) src.addFrom (0, 0, in, c, 0, inLen);
            src.applyGain (1.0f / (float) in.getNumChannels());
        }
        else
            src.copyFrom (0, 0, in, juce::jmin (ch, in.getNumChannels() - 1), 0, inLen);

        if (std::abs (ratio - 1.0) < 1e-9)
        {
            out.copyFrom (ch, 0, src, 0, 0, inLen);
            continue;
        }
        juce::LagrangeInterpolator interp;
        interp.reset();
        interp.process (ratio, src.getReadPointer (0), out.getWritePointer (ch), outLen, inLen, 0);
    }
    return out;
}

juce::Colour stemColour (const juce::String& n)
{
    if (n == "drums")  return juce::Colour (0xffffb04a);
    if (n == "bass")   return juce::Colour (0xffff5f7a);
    if (n == "other")  return juce::Colour (0xff6fe3ff);
    if (n == "vocals") return juce::Colour (0xffc48bff);
    if (n == "guitar") return juce::Colour (0xff7dff9a);
    if (n == "piano")  return juce::Colour (0xffffe66b);
    return juce::Colour (0xffc9d6e6);
}

// Threaded overlap-add driver, adapted from demucs.cpp's cli-apps/threaded_inference.hpp (MIT, sevagh).
static Eigen::Tensor3dXf threadedInference (const demucscpp::demucs_model& model, const Eigen::MatrixXf& full,
                                            int numThreads, const Progress& progress)
{
    const int SR = 44100;
    const int OV = (int) std::floor (SR * 0.75f);
    const int total = (int) full.cols();
    const int segLen = (int) std::ceil ((float) total / (float) numThreads);

    std::vector<float> threadProgress ((size_t) numThreads, 0.f);
    std::mutex m;
    std::vector<demucscpp::ProgressCallback> cbs;
    for (int i = 0; i < numThreads; ++i)
        cbs.push_back ([&, i] (float p, const std::string& msg)
        {
            float avg = 0.f;
            {
                std::lock_guard<std::mutex> l (m);
                threadProgress[(size_t) i] = p;
                for (auto v : threadProgress) avg += v;
                avg /= (float) numThreads;
            }
            if (progress) progress (avg, juce::String (msg));
        });

    std::vector<Eigen::MatrixXf> segments;
    for (int i = 0; i < numThreads; ++i)
    {
        const int start = i * segLen;
        const int end = std::min (total, start + segLen);
        Eigen::MatrixXf seg = Eigen::MatrixXf::Zero (2, end - start + 2 * OV);
        if (i == 0) seg.block (0, 0, 2, OV).colwise() = full.col (0);
        else        seg.block (0, 0, 2, OV) = full.block (0, start - OV, 2, OV);
        if (i == numThreads - 1)
        {
            const int rem = total - end;
            if (rem > 0) seg.block (0, end - start + OV, 2, rem) = full.block (0, end, 2, rem);
        }
        else
            seg.block (0, end - start + OV, 2, OV) = full.block (0, end, 2, OV);
        seg.block (0, OV, 2, end - start) = full.block (0, start, 2, end - start);
        segments.push_back (seg);
    }

    std::vector<Eigen::Tensor3dXf> outs ((size_t) numThreads);
    std::vector<std::thread> threads;
    for (int i = 0; i < numThreads; ++i)
        threads.emplace_back ([&, i] { outs[(size_t) i] = demucscpp::demucs_inference (model, segments[(size_t) i], cbs[(size_t) i]); });
    for (auto& t : threads) t.join();

    const int nSrc = model.is_4sources ? 4 : 6;
    Eigen::Tensor3dXf final (nSrc, 2, total);
    final.setZero();
    if (numThreads == 1)
    {
        for (int t = 0; t < nSrc; ++t)
            for (int ch = 0; ch < 2; ++ch)
                for (int j = 0; j < total; ++j)
                    final (t, ch, j) = outs[0](t, ch, j + OV);
        return final;
    }

    Eigen::VectorXf ramp (segLen);
    for (int i = 0; i < segLen; ++i) ramp (i) = (float) std::min (i + 1, segLen - i);
    ramp /= ramp.maxCoeff();
    Eigen::VectorXf sumW = Eigen::VectorXf::Zero (total);
    for (size_t i = 0; i < outs.size(); ++i)
    {
        const int segStart = (int) i * segLen;
        const int segCols = (int) segments[i].cols();
        for (int t = 0; t < nSrc; ++t)
            for (int ch = 0; ch < 2; ++ch)
                for (int j = 0; j < segCols; ++j)
                {
                    const int g = segStart + j - OV;
                    if (g < 0 || g >= total) continue;
                    float w = 1.f;
                    if (j < OV) w = ramp (j);
                    else if (j >= segLen) w = ramp (std::max (0, segCols - 1 - j));
                    final (t, ch, g) += outs[i](t, ch, j) * w;
                    if (t == 0 && ch == 0) sumW (g) += w;
                }
    }
    for (int t = 0; t < nSrc; ++t)
        for (int ch = 0; ch < 2; ++ch)
            for (int j = 0; j < total; ++j)
                if (sumW (j) > 0.f) final (t, ch, j) /= sumW (j);
    return final;
}

bool separate (const juce::AudioBuffer<float>& inBuf, double inRate, bool six, int threads,
               const Progress& progress, StemSet& out, juce::String& error)
{
    if (! modelAvailable (six)) { error = "Model file missing: " + modelFile (six).getFullPathName(); return false; }
    if (inBuf.getNumSamples() < 2048) { error = "Clip is too short"; return false; }

    if (progress) progress (0.f, "Loading model");
    auto model = std::make_unique<demucscpp::demucs_model>();
    if (! demucscpp::load_demucs_model (modelFile (six).getFullPathName().toStdString(), model.get()))
    { error = "Could not load model"; return false; }

    auto stereo = resample (inBuf, inRate, kModelRate, 2);
    const int n = stereo.getNumSamples();
    Eigen::MatrixXf audio (2, n);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < n; ++i)
            audio (ch, i) = stereo.getSample (ch, i);

    // demucs itself chunks in 7.8 s pieces; only split across threads when there is enough audio
    const int secs = n / 44100;
    threads = juce::jlimit (1, juce::jmax (1, threads), juce::jmax (1, secs / 8));

    Eigen::Tensor3dXf result = threadedInference (*model, audio, threads, progress);

    const int nSrc = six ? 6 : 4;
    static const char* names4[] = { "drums", "bass", "other", "vocals" };
    static const char* names6[] = { "drums", "bass", "other", "vocals", "guitar", "piano" };
    out.names.clear(); out.audio.clear();
    for (int t = 0; t < nSrc; ++t)
    {
        out.names.push_back (six ? names6[t] : names4[t]);
        juce::AudioBuffer<float> b (2, n);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < n; ++i)
                b.setSample (ch, i, result (t, ch, i));
        out.audio.push_back (std::move (b));
    }
    if (progress) progress (1.f, "Done");
    return true;
}

} // namespace Separator
