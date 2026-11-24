// From build-oneios-audio.py (stage_audio), written out by a helper of the port and kept by hand since.
/// DroidVMAudio.cpp's lines that change.
pub const AUDIO: &[(&str, &str)] = &[
    ("extern kmod_info_t g_kernel_kmod_info;\n",
     concat!("extern kmod_info_t g_kernel_kmod_info;\n",
             "// OneiOS (build-oneios-audio.py): the UIDs and the speaker's output latency, from boot arguments at attach.\n",
             "#include <pexpert/pexpert.h>\n",
             "static uint32_t oneios_output_latency = ", output_latency!(), ";\n",
             "static char oneios_out_uid[32] = \"Speaker\", oneios_in_uid[32] = \"Mic\";\n",
             "// OneiOS: the \"Actuator\" device (haptics): drivers/haptics/oneios_haptic.c takes its periods.\n",
             "extern \"C\" bool oneios_haptic_actuator_ready(void);\n",
             "extern \"C\" void oneios_haptic_actuator_published(bool yes);\n",
             "extern \"C\" void oneios_haptic_actuator_running(bool on);\n",
             "extern \"C\" uint32_t oneios_haptic_actuator_play(const int16_t *frames, uint32_t count, uint32_t channels);\n",
             "// OneiOS: what the Actuator's user clients do (the kinds are drivers/bridge/oneios_bridge.h's OH_EV_*).\n",
             "extern \"C\" void oneios_haptic_actuator_event(uint32_t kind, uint32_t a, uint32_t b);\n",
             "static uint32_t oneios_actuator_on = 1, oneios_actuator_channels = 1;\n",
             "static bool oneios_publishing_actuator;\n",
             "extern \"C\" { volatile uint64_t oneios_actuator_frames; }\n")),
    ("    bool isInput;\n",
     concat!("    bool isInput;\n",
             "    bool isActuator = false;   // OneiOS: the \"Actuator\" output: its periods go to drivers/haptics\n")),
    ("    uint32_t channels() const { return isInput ? InputChannels : OutputChannels; }\n",
     "    uint32_t channels() const { return isInput ? InputChannels : isActuator ? oneios_actuator_channels : OutputChannels; }\n"),
    ("        transportSet(!isInput, true);\n",
     "        if (isActuator) oneios_haptic_actuator_running(true); else transportSet(!isInput, true);\n"),
    ("        transportSet(!isInput, false);\n",
     "        if (isActuator) oneios_haptic_actuator_running(false); else transportSet(!isInput, false);\n"),
    ("            const uint32_t played = droidvm_snd_play(at, PeriodFrames);\n",
     "            const uint32_t played = isActuator ? oneios_haptic_actuator_play(at, PeriodFrames, ch) : droidvm_snd_play(at, PeriodFrames);\n"),
    ("            droidvm_audio_played = droidvm_audio_played + played;\n",
     concat!("            if (isActuator) oneios_actuator_frames = oneios_actuator_frames + played;\n",
             "            else droidvm_audio_played = droidvm_audio_played + played;\n")),
    ("        droidvm_audio_periods = droidvm_audio_periods + 1;\n",
     "        if (!isActuator) droidvm_audio_periods = droidvm_audio_periods + 1;\n"),
    ("    DroidVMAudioDevice *device = OSTypeAlloc(DroidVMAudioDevice);\n",
     concat!("    DroidVMAudioDevice *device = OSTypeAlloc(DroidVMAudioDevice);\n",
             "    if (device) device->isActuator = oneios_publishing_actuator;   // OneiOS: before initRole sizes the ring\n")),
    ("    setNumber(e, \"output latency\", Latency);\n",
     "    setNumber(e, \"output latency\", isActuator ? Latency : oneios_output_latency);   // OneiOS: the host path's latency\n"),
    (concat!("    bool speaker = publishDevice(resources, false, \"Speaker\", \"droidvm-audio-out\");\n",
             "    bool mic = publishDevice(resources, true, \"Mic\", \"droidvm-audio-in\");\n"),
     concat!("    // OneiOS: oneios-audio-out-uid, oneios-audio-in-uid (none: no microphone), oneios-audio-latency.\n",
             "    PE_parse_boot_arg_str(\"oneios-audio-out-uid\", oneios_out_uid, sizeof(oneios_out_uid));\n",
             "    PE_parse_boot_arg_str(\"oneios-audio-in-uid\", oneios_in_uid, sizeof(oneios_in_uid));\n",
             "    PE_parse_boot_argn(\"oneios-audio-latency\", &oneios_output_latency, sizeof(oneios_output_latency));\n",
             "    bool speaker = publishDevice(resources, false, oneios_out_uid, \"droidvm-audio-out\");\n",
             "    bool mic = strcmp(oneios_in_uid, \"none\") != 0 && publishDevice(resources, true, oneios_in_uid, \"droidvm-audio-in\");\n",
             "    // OneiOS: the Actuator (haptics), when drivers/haptics has the host's actuator; oneios-actuator=0 leaves it out.\n",
             "    PE_parse_boot_argn(\"oneios-actuator\", &oneios_actuator_on, sizeof(oneios_actuator_on));\n",
             "    PE_parse_boot_argn(\"oneios-actuator-channels\", &oneios_actuator_channels, sizeof(oneios_actuator_channels));\n",
             "    if (oneios_actuator_channels != 2) oneios_actuator_channels = 1;\n",
             "    oneios_publishing_actuator = true;\n",
             "    bool actuator = oneios_actuator_on && oneios_haptic_actuator_ready() && publishDevice(resources, false, \"Actuator\", \"oneios-actuator\");\n",
             "    oneios_publishing_actuator = false;\n",
             "    oneios_haptic_actuator_published(actuator);\n")),
    ("    IOLog(\"DroidVMAudio: published Speaker=%d Mic=%d as IOAudio2Devices\\n\", speaker, mic);\n",
     concat!("    IOLog(\"DroidVMAudio: published Speaker=%d Mic=%d Actuator=%d as IOAudio2Devices (UIDs %s, %s, Actuator x%u; output latency %u frames)\\n\", speaker, mic,\n",
             "          actuator, oneios_out_uid, oneios_in_uid, oneios_actuator_channels, oneios_output_latency);\n")),
    // fix-haptics (2026-11-13): the Actuator's user clients, for drivers/haptics' log lines and counter line: who opens
    // it, which selectors it gets and refuses, which format the HAL asks for, which memory it maps (OH_EV_* 0..5).
    ("    droidvm_audio_clients = droidvm_audio_clients + 1;\n",
     concat!("    droidvm_audio_clients = droidvm_audio_clients + 1;\n",
             "    if (device->isActuator) oneios_haptic_actuator_event(0, 0, 0);   // OneiOS: opened\n")),
    ("    droidvm_audio_clients = droidvm_audio_clients - 1;\n",
     concat!("    droidvm_audio_clients = droidvm_audio_clients - 1;\n",
             "    if (device && device->isActuator) oneios_haptic_actuator_event(1, 0, 0);   // OneiOS: closed\n")),
    ("    droidvm_audio_selectors = droidvm_audio_selectors + 1;\n",
     concat!("    droidvm_audio_selectors = droidvm_audio_selectors + 1;\n",
             "    if (device && device->isActuator) oneios_haptic_actuator_event(2, selector, 0);   // OneiOS: a selector\n")),
    ("        droidvm_audio_bad_selectors = droidvm_audio_bad_selectors + 1;\n",
     concat!("        droidvm_audio_bad_selectors = droidvm_audio_bad_selectors + 1;\n",
             "        if (device && device->isActuator) oneios_haptic_actuator_event(3, selector, 0);   // OneiOS: refused\n")),
    ("        if (a->scalarInputCount < 1 || a->structureInputSize != sizeof f) return kIOReturnBadArgument;\n",
     concat!("        if (a->scalarInputCount < 1 || a->structureInputSize != sizeof f) {\n",
             "            if (device->isActuator) oneios_haptic_actuator_event(3, selector, 0);   // OneiOS: refused (its size)\n",
             "            return kIOReturnBadArgument;\n",
             "        }\n")),
    ("        memcpy(&f, a->structureInput, sizeof f);\n",
     concat!("        memcpy(&f, a->structureInput, sizeof f);\n",
             "        if (device->isActuator)   // OneiOS: the format the HAL asks of the Actuator\n",
             "            oneios_haptic_actuator_event(4, f.channels | device->channels() << 16, uint32_t(f.rate >> 32));\n")),
    ("    droidvm_audio_maps = droidvm_audio_maps + 1;\n",
     concat!("    droidvm_audio_maps = droidvm_audio_maps + 1;\n",
             "    if (device->isActuator) oneios_haptic_actuator_event(5, type, 0);   // OneiOS: a mapping\n")),
    // fix-audio-latency (2026-11-15): the speaker's latency inside the guest, and the ring's wrap (every device).
    //  - iOS's I/O buffer: the HAL caps it at 0.375 of the device's "io buffer frame size" (CoreAudio's
    //    HALS_IOContextDescription::CalculateMaximumIOBufferFrameSize: the smallest ring of the context x 0.375, at
    //    most 4096), and system sounds take the cap: 3072 frames (64 ms) of the 8192-frame ring. The speaker's ring is
    //    oneios-audio-ring=FRAMES (default 1920, 8 periods: at most 720 frames, 15 ms).
    //  - The speaker hands a period on as it starts (the HAL has written it by then: its safety offset is that
    //    period, 240 frames, not 480), so the device clock is the handoff and the output latency is the host's alone.
    //  - A period that runs over the ring's end goes through a bounce buffer in two parts. 8192 is not a whole
    //    number of 240-frame periods: the period at 8160 read and zeroed (the microphone's wrote) 208 frames past the
    //    ring's buffer, and the ring's first 208 frames were never played. The zero time stamp moves at each wrap
    //    (the multiple of the ring the last period crossed), not only when a period ends on one (every 245760 frames).
    //  - The speaker engine's timing for OneiOSAudio.c's report: how late it took each period, how far ahead the HAL
    //    has written (EngineStatus +0x20), how often that came within a period of the ring's end.
    ("uint32_t droidvm_snd_capture(int16_t *frames, uint32_t count);\n",
     concat!("uint32_t droidvm_snd_capture(int16_t *frames, uint32_t count);\n",
             "// OneiOS (fix-audio-latency): the speaker's ring (oneios-audio-ring) and its engine's timing (OneiOSAudio.c reports it).\n",
             "uint32_t oneios_speaker_ring = 1920;\n",
             "volatile uint64_t oneios_speaker_periods, oneios_speaker_late_sum, oneios_speaker_late_max, oneios_speaker_leads,\n",
             "    oneios_speaker_lead_sum, oneios_speaker_lead_min = ~0ULL, oneios_speaker_lead_max, oneios_speaker_lead_last, oneios_speaker_tight;\n")),
    ("    uint32_t seed;\n",
     concat!("    uint32_t seed;\n",
             "    uint32_t ringFrames = RingFrames;   // OneiOS: the speaker's is oneios_speaker_ring\n",
             "    bool speakerEngine() const { return !isInput && !isActuator; }   // OneiOS: hands a period on as it starts\n",
             "    void oneiosSpeakerPeriod(uint64_t deadline, uint64_t done);\n")),
    ("    ring = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, Shared, RingFrames * 2 * channels(), page_size);\n",
     concat!("    ringFrames = speakerEngine() ? oneios_speaker_ring : RingFrames;   // OneiOS\n",
             "    ring = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, Shared, ringFrames * 2 * channels(), page_size);\n")),
    ("    setNumber(e, \"output safety offset\", SafetyOffset);\n",
     "    setNumber(e, \"output safety offset\", speakerEngine() ? PeriodFrames : SafetyOffset);   // OneiOS: written by the period's start\n"),
    ("    setNumber(e, \"io buffer frame size\", RingFrames);\n",
     "    setNumber(e, \"io buffer frame size\", ringFrames);   // OneiOS: iOS's I/O buffer is at most 0.375 of it\n"),
    ("void DroidVMAudioDevice::engineMain(void *self, wait_result_t) { static_cast<DroidVMAudioDevice *>(self)->run(); }\n",
     concat!("void DroidVMAudioDevice::engineMain(void *self, wait_result_t) { static_cast<DroidVMAudioDevice *>(self)->run(); }\n",
             "\n",
             "// OneiOS (fix-audio-latency): the speaker at the start of its period `done` (the deadline has passed): the zero time\n",
             "// stamp of a wrap the last period crossed (in the past now, as a DMA wrap's), and the timing counters: how late the\n",
             "// period was taken (us), how far ahead of it the HAL has written (its write end: iOS's I/O buffer and safety offset as\n",
             "// used), and whether that came within a period of overwriting what is not yet taken (the ring too small for them).\n",
             "void DroidVMAudioDevice::oneiosSpeakerPeriod(uint64_t deadline, uint64_t done) {\n",
             "    EngineStatus *s = status();\n",
             "    const uint64_t wrap = done / ringFrames * ringFrames;\n",
             "    if (done && wrap + PeriodFrames > done) {\n",
             "        s->hostTime = startTime + absolute(wrap);\n",
             "        __asm__ volatile(\"dmb ish\" ::: \"memory\");\n",
             "        s->sampleTime = wrap;\n",
             "        __asm__ volatile(\"dmb ish\" ::: \"memory\");\n",
             "    }\n",
             "    const uint64_t now = mach_absolute_time();\n",
             "    uint64_t late = 0;\n",
             "    if (now > deadline) absolutetime_to_nanoseconds(now - deadline, &late);\n",
             "    late /= 1000;\n",
             "    oneios_speaker_periods = oneios_speaker_periods + 1;\n",
             "    oneios_speaker_late_sum = oneios_speaker_late_sum + late;\n",
             "    if (late > oneios_speaker_late_max) oneios_speaker_late_max = late;\n",
             "    const uint64_t written = s->writeEnd;\n",
             "    oneios_speaker_lead_last = written;\n",
             "    if (written <= done) return;\n",
             "    const uint64_t lead = written - done;\n",
             "    oneios_speaker_leads = oneios_speaker_leads + 1;\n",
             "    oneios_speaker_lead_sum = oneios_speaker_lead_sum + lead;\n",
             "    if (lead < oneios_speaker_lead_min) oneios_speaker_lead_min = lead;\n",
             "    if (lead > oneios_speaker_lead_max) oneios_speaker_lead_max = lead;\n",
             "    if (lead + PeriodFrames > ringFrames) oneios_speaker_tight = oneios_speaker_tight + 1;\n",
             "}\n")),
    ("        const uint64_t deadline = startTime + absolute(done + PeriodFrames);\n",
     concat!("        // OneiOS: the speaker takes a period as it starts (the HAL has written it a safety offset of one period\n",
             "        // ahead), the others once it has passed.\n",
             "        const bool early = speakerEngine();\n",
             "        const uint64_t deadline = startTime + absolute(done + (early ? 0 : PeriodFrames));\n")),
    ("        int16_t *at = buffer + uint32_t(done % RingFrames) * ch;\n",
     concat!("        if (early) oneiosSpeakerPeriod(deadline, done);\n",
             "        // OneiOS: a period over the ring's end goes through `wrapped`, in two parts.\n",
             "        int16_t wrapped[PeriodFrames * 2];\n",
             "        const uint32_t first = uint32_t(done % ringFrames), part = ringFrames - first < PeriodFrames ? ringFrames - first : PeriodFrames;\n",
             "        int16_t *at = part == PeriodFrames ? buffer + first * ch : wrapped;\n",
             "        if (at == wrapped && !isInput) {\n",
             "            memcpy(wrapped, buffer + first * ch, part * 2 * ch);\n",
             "            memcpy(wrapped + part * ch, buffer, (PeriodFrames - part) * 2 * ch);\n",
             "        }\n")),
    ("        done += PeriodFrames;\n",
     concat!("        if (at == wrapped) {   // OneiOS: back into the ring (the microphone's frames; the speaker's silence)\n",
             "            memcpy(buffer + first * ch, wrapped, part * 2 * ch);\n",
             "            memcpy(buffer, wrapped + part * ch, (PeriodFrames - part) * 2 * ch);\n",
             "        }\n",
             "        done += PeriodFrames;\n")),
    ("        if (done % RingFrames == 0) {\n",
     concat!("        // OneiOS: at the wrap the period crossed (the speaker's is set as its next period starts).\n",
             "        const uint64_t wrap = done / ringFrames * ringFrames;\n",
             "        if (!early && wrap + PeriodFrames > done) {\n")),
    ("            s->hostTime = startTime + absolute(done);\n",
     "            s->hostTime = startTime + absolute(wrap);\n"),
    ("            s->sampleTime = done;\n",
     "            s->sampleTime = wrap;\n"),
    ("    droidvm_audio_stage = 3;\n",
     concat!("    // OneiOS: oneios-audio-ring=FRAMES, the speaker's ring (960..8192; best a whole number of 240-frame periods).\n",
             "    PE_parse_boot_argn(\"oneios-audio-ring\", &oneios_speaker_ring, sizeof(oneios_speaker_ring));\n",
             "    if (oneios_speaker_ring < 4 * PeriodFrames) oneios_speaker_ring = 4 * PeriodFrames;\n",
             "    if (oneios_speaker_ring > RingFrames) oneios_speaker_ring = RingFrames;\n",
             "    droidvm_audio_stage = 3;\n")),
    ("    droidvm_audio_stage = 4;\n",
     concat!("    droidvm_audio_stage = 4;\n",
             "    IOLog(\"DroidVMAudio: speaker: ring %u frames (iOS's I/O buffer at most %u), safety offset %u, each period handed on as it \"\n",
             "          \"starts; output latency %u frames\\n\", oneios_speaker_ring, oneios_speaker_ring * 3 / 8, (unsigned)PeriodFrames, oneios_output_latency);\n")),
    // fix-audio-volume (2026-11-22): the speaker's hardware volume. VirtualAudio's speaker route has the software volume
    // mode HardwareOnly, and a physical device without a volume control made audiomxd throw at every volume set
    // ("Volume Mode is HardwareOnly but physical device does not support HW volume", OutputVolumeControl_HAL_Common.cpp
    // :1423, 10-17 times a boot): no volume, no HUD, the keys and the slider changed nothing. The Speaker now publishes
    // a volume control in its "controls" (the HAL's IOAudio2 description, read in 24A446's CoreAudio:
    // HALS_IOA2Control::SynchronizeWithDictionary, HALS_IOA2UCDevice::SetControlValue), takes the HAL's SetControlValue
    // (selector 2: control ID and value in, the value taken out) and scales its periods by it before the transport,
    // so iOS's volume is the one that counts (the host's helper plays at full scale, runtime/bridges/audio).
    ("uint32_t oneios_speaker_ring = 1920;\n",
     concat!("uint32_t oneios_speaker_ring = 1920;\n",
             "// OneiOS (fix-audio-volume): the speaker's volume step (0..128, VirtualAudio's), its gain (Q16) and how often it changed.\n",
             "volatile uint32_t oneios_speaker_volume = 128, oneios_speaker_gain = 65536, oneios_speaker_volume_sets;\n")),
    ("uint64_t absolute(uint64_t frames) { uint64_t t; nanoseconds_to_absolutetime(frames * 1000000000ULL / Rate, &t); return t; }\n",
     concat!("uint64_t absolute(uint64_t frames) { uint64_t t; nanoseconds_to_absolutetime(frames * 1000000000ULL / Rate, &t); return t; }\n",
             "// OneiOS (fix-audio-volume): the speaker's volume control as the HAL reads it (24A446 CoreAudio's HALS_IOA2Control):\n",
             "// class 'vlme', base class 'levl', output scope, main element; integers 0..128 over one range from -64 dB in 0.5 dB\n",
             "// steps (dB values Fixed64, as the sample rates). Step 0 is silence, 128 is 0 dB (the stream as iOS made it).\n",
             "enum : uint32_t { OneiosVolumeID = 1, OneiosVolumeSteps = 128 };\n",
             "const uint32_t oneios_volume_gain[OneiosVolumeSteps + 1] = {   // Q16: 65536 x 10^((step / 2 - 64) / 20), 0 at step 0\n",
             "    0, 44, 46, 49, 52, 55, 58, 62, 66, 69, 74, 78, 83, 87, 93, 98,\n",
             "    104, 110, 117, 123, 131, 139, 147, 155, 165, 174, 185, 196, 207, 220, 233, 246,\n",
             "    261, 276, 293, 310, 328, 348, 369, 390, 414, 438, 464, 491, 521, 551, 584, 619,\n",
             "    655, 694, 735, 779, 825, 874, 926, 981, 1039, 1100, 1165, 1234, 1308, 1385, 1467, 1554,\n",
             "    1646, 1744, 1847, 1957, 2072, 2195, 2325, 2463, 2609, 2764, 2927, 3101, 3285, 3479, 3685, 3904,\n",
             "    4135, 4380, 4640, 4915, 5206, 5514, 5841, 6187, 6554, 6942, 7353, 7789, 8250, 8739, 9257, 9806,\n",
             "    10387, 11002, 11654, 12345, 13076, 13851, 14672, 15541, 16462, 17437, 18471, 19565, 20724, 21952, 23253, 24631,\n",
             "    26090, 27636, 29274, 31008, 32846, 34792, 36854, 39037, 41350, 43801, 46396, 49145, 52057, 55142, 58409, 61870,\n",
             "    65536,\n",
             "};\n",
             "OSArray *oneiosControls(bool volume) {\n",
             "    OSArray *list = OSArray::withCapacity(1);\n",
             "    if (!list || !volume) return list;\n",
             "    OSDictionary *c = OSDictionary::withCapacity(12), *range = OSDictionary::withCapacity(4);\n",
             "    OSArray *map = OSArray::withCapacity(1);\n",
             "    if (c && range && map) {\n",
             "        setNumber(c, \"control ID\", OneiosVolumeID);\n",
             "        setNumber(c, \"base class\", 0x6c65766c);   // 'levl'\n",
             "        setNumber(c, \"class\", 0x766c6d65);        // 'vlme'\n",
             "        setNumber(c, \"scope\", 0x6f757470);        // 'outp'\n",
             "        setNumber(c, \"element\", 0);\n",
             "        c->setObject(\"read only\", kOSBooleanFalse);\n",
             "        setNumber(c, \"value\", oneios_speaker_volume);\n",
             "        setNumber(c, \"minimum value\", 0);\n",
             "        setNumber(c, \"maximum value\", OneiosVolumeSteps);\n",
             "        setNumber(range, \"start int value\", 0);\n",
             "        setNumber(range, \"start db value\", uint64_t(int64_t(-64) * (int64_t(1) << 32)), 64);\n",
             "        setNumber(range, \"integer steps\", OneiosVolumeSteps);\n",
             "        setNumber(range, \"db per step\", uint64_t(1) << 31, 64);   // 0.5 dB\n",
             "        map->setObject(range);\n",
             "        c->setObject(\"range map\", map);\n",
             "        list->setObject(c);\n",
             "    }\n",
             "    OSSafeReleaseNULL(c); OSSafeReleaseNULL(range); OSSafeReleaseNULL(map);\n",
             "    return list;\n",
             "}\n",
             "// One period of the speaker at the volume's gain, in place (480 multiplies a 5 ms period; nothing at 0 dB).\n",
             "void oneiosVolume(int16_t *s, uint32_t n) {\n",
             "    const int32_t g = int32_t(oneios_speaker_gain);\n",
             "    if (g >= 65536) return;\n",
             "    for (uint32_t i = 0; i < n; i++) s[i] = int16_t((int32_t(s[i]) * g) >> 16);\n",
             "}\n")),
    ("    if (OSArray *a = OSArray::withCapacity(1)) { e->setProperty(\"controls\", a); a->release(); }\n",
     "    if (OSArray *a = oneiosControls(speakerEngine())) { e->setProperty(\"controls\", a); a->release(); }   // OneiOS: the speaker's volume\n"),
    ("    void oneiosSpeakerPeriod(uint64_t deadline, uint64_t done);\n",
     concat!("    void oneiosSpeakerPeriod(uint64_t deadline, uint64_t done);\n",
             "    uint32_t oneiosSetVolume(uint64_t value, IORegistryEntry *client);   // OneiOS: the speaker's volume control\n")),
    ("    if (lead + PeriodFrames > ringFrames) oneios_speaker_tight = oneios_speaker_tight + 1;\n}\n",
     concat!("    if (lead + PeriodFrames > ringFrames) oneios_speaker_tight = oneios_speaker_tight + 1;\n}\n",
             "\n",
             "// OneiOS (fix-audio-volume): VirtualAudio's volume through the HAL's SetControlValue: the step (clamped to 0..128), its\n",
             "// gain for the engine, and the description again (on the device and the client: the HAL reads it through both). The\n",
             "// first 16 changes are logged, then every 64th.\n",
             "uint32_t DroidVMAudioDevice::oneiosSetVolume(uint64_t value, IORegistryEntry *client) {\n",
             "    const int32_t asked = int32_t(uint32_t(value));\n",
             "    const uint32_t v = asked < 0 ? 0 : asked > int32_t(OneiosVolumeSteps) ? OneiosVolumeSteps : uint32_t(asked);\n",
             "    const bool changed = v != oneios_speaker_volume;\n",
             "    oneios_speaker_gain = oneios_volume_gain[v];\n",
             "    oneios_speaker_volume = v;\n",
             "    if (OSArray *a = oneiosControls(true)) {\n",
             "        setProperty(\"controls\", a);\n",
             "        if (client) client->setProperty(\"controls\", a);\n",
             "        a->release();\n",
             "    }\n",
             "    if (changed) {\n",
             "        const uint32_t n = oneios_speaker_volume_sets + 1;   // no read of a volatile assignment (C++20)\n",
             "        oneios_speaker_volume_sets = n;\n",
             "        const uint32_t down = OneiosVolumeSteps - v;   // half dB below 0 dB\n",
             "        if (n <= 16 || n % 64 == 0)\n",
             "            IOLog(\"DroidVMAudio: speaker volume %u of %u (%s%u.%u dB, gain %u of 65536), change %u: iOS's volume, applied here \"\n",
             "                  \"(the host plays at full scale)\\n\", v, (unsigned)OneiosVolumeSteps, v == 0 ? \"silent, -\" : down ? \"-\" : \"\",\n",
             "                  down / 2, down % 2 * 5, oneios_speaker_gain, n);\n",
             "    }\n",
             "    return v;\n",
             "}\n")),
    (concat!("    case 2:                                                // SetControlValue: no controls are published\n",
             "        return kIOReturnBadArgument;\n"),
     concat!("    case 2: {                                              // SetControlValue: the speaker's volume (OneiOS)\n",
             "        if (!device->speakerEngine() || a->scalarInputCount < 2 || a->scalarInput[0] != OneiosVolumeID) return kIOReturnBadArgument;\n",
             "        const uint32_t v = device->oneiosSetVolume(a->scalarInput[1], this);\n",
             "        if (a->scalarOutputCount >= 1) { a->scalarOutput[0] = v; a->scalarOutputCount = 1; }\n",
             "        return kIOReturnSuccess;\n",
             "    }\n")),
    ("            const uint32_t played = isActuator ? oneios_haptic_actuator_play(at, PeriodFrames, ch) : droidvm_snd_play(at, PeriodFrames);\n",
     concat!("            if (!isActuator) oneiosVolume(at, PeriodFrames * ch);   // OneiOS: iOS's volume (the speaker's control)\n",
             "            const uint32_t played = isActuator ? oneios_haptic_actuator_play(at, PeriodFrames, ch) : droidvm_snd_play(at, PeriodFrames);\n")),
];

/// The native kernel's virtio-snd on crosvm's PCI (xnuloader-crosvm --audio): Droid-VM's crosvm has no virtio-mmio sound
/// card, so the card is a virtio-pci function of its ECAM host, and crosvm picks its device (`--virtio-snd` has no
/// pci-address). /arm-io/virtio-snd ("oneios,virtio-snd-pci") is all of bus 0, and the transport takes the first
/// 1af4:1059 function there. DroidVMVirtioSnd.c's lines that change for it, compiled only with XNU_ONEIOS_NATIVE: the
/// MMIO transport's register offsets go to the function's regions (VirtIO 1.2 4.1.4), as the GPU's do (gpu.rs
/// TRANSPORT_PCI), with the device configuration (the stream count at 0x104) and the four queues' notifications. The
/// queues and periods stay in the transport's DMA region of the guest's RAM, which the shared-RAM VM's host reads; polled,
/// as before. The hosted kernel compiles the file as before.
pub const SND_TRANSPORT_PCI: &[(&str, &str)] = &[
    (r#"// A trapped MMIO access must be a single load/store with a plain base register (see DroidVMVirtioInput.c).
static uint32_t snd_read32(unsigned offset) {
    uint32_t value;
    __asm__ volatile("ldr %w0, [%1]" : "=r"(value) : "r"(snd.registers + offset) : "memory");
    return value;
}
static void snd_write32(unsigned offset, uint32_t value) {
    __asm__ volatile("str %w0, [%1]\n\tdsb oshst" :: "r"(value), "r"(snd.registers + offset) : "memory");
}
"#,
     r#"#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE
// OneiOS native (staged/audio.rs SND_TRANSPORT_PCI): the card as a virtio-pci function of crosvm's ECAM host
// (/arm-io/virtio-snd "oneios,virtio-snd-pci" is bus 0, xnuloader-crosvm --audio), the MMIO transport's offsets
// (VirtIO 1.2 4.2.2) on its regions (4.1.4), as the GPU's (staged/gpu.rs TRANSPORT_PCI). Single loads and stores with a
// plain base register, as the MMIO accessors.
extern int printf(const char *, ...);
static bool snd_pci;
static uintptr_t snd_pci_common, snd_pci_notify, snd_pci_device;
static uint32_t snd_pci_multiplier;
static uint16_t snd_pci_notify_off[4];
static uint32_t snd_pci_rd32(uintptr_t p) { uint32_t v; __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(p) : "memory"); return v; }
static uint16_t snd_pci_rd16(uintptr_t p) { uint32_t v; __asm__ volatile("ldrh %w0, [%1]" : "=r"(v) : "r"(p) : "memory"); return (uint16_t)v; }
static uint8_t snd_pci_rd8(uintptr_t p) { uint32_t v; __asm__ volatile("ldrb %w0, [%1]" : "=r"(v) : "r"(p) : "memory"); return (uint8_t)v; }
static void snd_pci_wr32(uintptr_t p, uint32_t v) { __asm__ volatile("str %w0, [%1]\n\tdsb oshst" :: "r"(v), "r"(p) : "memory"); }
static void snd_pci_wr16(uintptr_t p, uint16_t v) { __asm__ volatile("strh %w0, [%1]\n\tdsb oshst" :: "r"((uint32_t)v), "r"(p) : "memory"); }
static void snd_pci_wr8(uintptr_t p, uint8_t v) { __asm__ volatile("strb %w0, [%1]\n\tdsb oshst" :: "r"((uint32_t)v), "r"(p) : "memory"); }
static uint32_t snd_pci_read(unsigned offset) {
    if (offset >= 0x100) return snd_pci_rd32(snd_pci_device + (offset - 0x100));   // the card's configuration
    switch (offset) {
    case 0x00: return 0x74726976;                                // MagicValue, Version, DeviceID: the function is 1af4:1059
    case 0x04: return 2;
    case 0x08: return 25;
    case 0x10: return snd_pci_rd32(snd_pci_common + 0x04);       // DeviceFeatures, of the word DeviceFeaturesSel chose
    case 0x34: return snd_pci_rd16(snd_pci_common + 0x18);       // QueueNumMax: the selected queue's size until it is set
    case 0x44: return snd_pci_rd16(snd_pci_common + 0x1c);       // QueueReady: queue_enable
    case 0x70: return snd_pci_rd8(snd_pci_common + 0x14);        // Status
    default: return 0;
    }
}
static void snd_pci_write(unsigned offset, uint32_t value) {
    switch (offset) {
    case 0x14: snd_pci_wr32(snd_pci_common + 0x00, value); break;   // DeviceFeaturesSel
    case 0x20: snd_pci_wr32(snd_pci_common + 0x0c, value); break;   // DriverFeatures (the driver keeps ACCESS_PLATFORM)
    case 0x24: snd_pci_wr32(snd_pci_common + 0x08, value); break;   // DriverFeaturesSel
    case 0x30:                                                      // QueueSel, and that queue's notification offset
        snd_pci_wr16(snd_pci_common + 0x16, (uint16_t)value);
        if (value < 4) snd_pci_notify_off[value] = snd_pci_rd16(snd_pci_common + 0x1e);
        break;
    case 0x38: snd_pci_wr16(snd_pci_common + 0x18, (uint16_t)value); break;
    case 0x44: snd_pci_wr16(snd_pci_common + 0x1c, (uint16_t)value); break;
    case 0x50:                                                      // QueueNotify
        if (value < 4) snd_pci_wr16(snd_pci_notify + (uintptr_t)snd_pci_notify_off[value] * snd_pci_multiplier, (uint16_t)value);
        break;
    case 0x70: snd_pci_wr8(snd_pci_common + 0x14, (uint8_t)value); break;
    case 0x80: case 0x84: snd_pci_wr32(snd_pci_common + 0x20 + (offset - 0x80), value); break;   // queue_desc
    case 0x90: case 0x94: snd_pci_wr32(snd_pci_common + 0x28 + (offset - 0x90), value); break;   // queue_driver
    case 0xa0: case 0xa4: snd_pci_wr32(snd_pci_common + 0x30 + (offset - 0xa0), value); break;   // queue_device
    default: break;
    }
}
// The function's regions from its vendor capabilities: common configuration (1), notifications (2) and the card's
// configuration (4), each in the BAR crosvm assigned, mapped as device memory; the ISR (3) is not read (polled). False
// when one is missing.
static bool snd_pci_regions(uintptr_t config) {
    if (!(snd_pci_rd16(config + 0x06) & 0x10)) return false;                    // no capabilities list
    struct { uint8_t bar; uint32_t offset, length; bool found; } r[5];
    memset(r, 0, sizeof(r));
    uint32_t multiplier = 0;
    uint8_t at = snd_pci_rd8(config + 0x34) & 0xfc;
    for (unsigned walked = 0; at >= 0x40 && at <= 0x100 - 20 && walked < 48; ++walked) {
        const uint8_t id = snd_pci_rd8(config + at), next = snd_pci_rd8(config + at + 1) & 0xfc, type = snd_pci_rd8(config + at + 3);
        if (id == 0x09 && type >= 1 && type <= 4 && !r[type].found) {
            r[type].bar = snd_pci_rd8(config + at + 4);
            r[type].offset = snd_pci_rd32(config + at + 8);
            r[type].length = snd_pci_rd32(config + at + 12);
            r[type].found = true;
            if (type == 2) multiplier = snd_pci_rd32(config + at + 16);
        }
        at = next;
    }
    if (!r[1].found || !r[2].found || !r[4].found || r[1].length < 0x38 || r[2].length < 2 || r[4].length < 12) return false;
    snd_pci_wr16(config + 0x04, (uint16_t)(snd_pci_rd16(config + 0x04) | 6));   // memory space and bus master
    uintptr_t mapped[5] = {0};
    for (unsigned t = 1; t <= 4; ++t) {
        if (t == 3) continue;
        const unsigned bar = r[t].bar;
        if (bar > 5 || r[t].offset > UINT32_MAX - r[t].length) return false;
        const uint32_t low = snd_pci_rd32(config + 0x10 + bar * 4);
        if (low & 1) return false;                                              // an I/O BAR
        uint64_t base = low & ~(uint64_t)0xf;
        if ((low & 6) == 4) base |= (uint64_t)snd_pci_rd32(config + 0x10 + bar * 4 + 4) << 32;
        if (!base) return false;
        const uint64_t from = (base + r[t].offset) & ~(uint64_t)0x3fff;
        const uint64_t to = (base + r[t].offset + r[t].length + 0x3fff) & ~(uint64_t)0x3fff;
        const uintptr_t m = ml_io_map((vm_offset_t)from, (vm_size_t)(to - from));
        if (!m) return false;
        mapped[t] = m + (uintptr_t)(base + r[t].offset - from);
    }
    snd_pci_common = mapped[1]; snd_pci_notify = mapped[2]; snd_pci_device = mapped[4];
    snd_pci_multiplier = multiplier;
    return true;
}
// The bus's first 1af4:1059 function (function 0 of each device: 32 KiB a device on crosvm's ECAM) with its regions
// mapped: its configuration space, or 0.
static uintptr_t snd_pci_find(vm_offset_t bus, uint64_t size) {
    const uintptr_t space = ml_io_map(bus, (vm_size_t)size);
    if (!space) { printf("droidvm-snd: the PCI bus at %#llx not mapped\n", (unsigned long long)bus); return 0; }
    for (unsigned dev = 0; dev < 32 && ((uint64_t)dev << 15) + 0x1000 <= size; ++dev) {
        const uintptr_t config = space + ((uintptr_t)dev << 15);
        if (snd_pci_rd32(config) != (0x1af4u | 0x1059u << 16)) continue;
        if (!snd_pci_regions(config)) { printf("droidvm-snd: the sound card at 00:%02x.0 has not its regions\n", dev); return 0; }
        snd_pci = true;
        printf("droidvm-snd: virtio-pci sound card at 00:%02x.0 (1af4:1059), notifications x%u, polled\n", dev, snd_pci_multiplier);
        return config;
    }
    printf("droidvm-snd: no 1af4:1059 function on bus 0 (crosvm without --virtio-snd?)\n");
    return 0;
}
static uint32_t snd_read32(unsigned offset) {
    if (snd_pci) return snd_pci_read(offset);
    uint32_t value;
    __asm__ volatile("ldr %w0, [%1]" : "=r"(value) : "r"(snd.registers + offset) : "memory");
    return value;
}
static void snd_write32(unsigned offset, uint32_t value) {
    if (snd_pci) { snd_pci_write(offset, value); return; }
    __asm__ volatile("str %w0, [%1]\n\tdsb oshst" :: "r"(value), "r"(snd.registers + offset) : "memory");
}
#else
// A trapped MMIO access must be a single load/store with a plain base register (see DroidVMVirtioInput.c).
static uint32_t snd_read32(unsigned offset) {
    uint32_t value;
    __asm__ volatile("ldr %w0, [%1]" : "=r"(value) : "r"(snd.registers + offset) : "memory");
    return value;
}
static void snd_write32(unsigned offset, uint32_t value) {
    __asm__ volatile("str %w0, [%1]\n\tdsb oshst" :: "r"(value), "r"(snd.registers + offset) : "memory");
}
#endif
"#),
    ("                snd.registers = ml_io_map((vm_offset_t)(reg[0] - ranges[0] + ranges[1]), 0x1000);\n",
     r#"#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE
                // OneiOS native: on crosvm's ECAM bus ("oneios,virtio-snd-pci", xnuloader-crosvm --audio) its 1af4:1059
                // function, wherever crosvm put it; an MMIO window as before
                if (SecureDTGetProperty(node, "compatible", &value, &size) == kSuccess && size >= 22 &&
                    !memcmp(value, "oneios,virtio-snd-pci", 22))
                    snd.registers = snd_pci_find((vm_offset_t)(reg[0] - ranges[0] + ranges[1]), reg[1]);
                else
#endif
                snd.registers = ml_io_map((vm_offset_t)(reg[0] - ranges[0] + ranges[1]), 0x1000);
"#),
    // A request the device answered after its 2 s wait: its reply is taken off the used ring before the next request, which
    // otherwise read that late reply as its own (take7: Android's audio server, One UI stopped, took 10 s to open the
    // microphone at its PREPARE; the speaker's START then looked refused, the stream stayed started on the device and every
    // later START was "START to START", NOT_SUPP, so no period reached the phone).
    ("static bool control(uint32_t request_size, uint32_t reply_size) {\n",
     r#"#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE
// OneiOS native (staged/audio.rs SND_TRANSPORT_PCI): requests the device answered after their wait; their replies are taken
// off the used ring before the next request, so that each request reads its own reply.
static uint16_t snd_control_late;
#endif
static bool control(uint32_t request_size, uint32_t reply_size) {
#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE
    while (snd_control_late && used(SND_CONTROL)[1] != snd.used[SND_CONTROL]) { snd_barrier(); ++snd.used[SND_CONTROL]; --snd_control_late; }
    if (snd_control_late) { droidvm_snd_errors++; return false; }   // still unanswered: the queue's one chain is the device's
#endif
"#),
    ("        if (waited == 2000) { droidvm_snd_errors++; return false; }   // 2 s\n",
     r#"#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE
        if (waited == 2000) { droidvm_snd_errors++; snd_control_late++; return false; }   // 2 s; its reply is taken later
#else
        if (waited == 2000) { droidvm_snd_errors++; return false; }   // 2 s
#endif
"#),
    // A START that failed (a late reply of its own) may have left the stream started on the device: STOP and START once more.
    ("            snd.out_running = pcm_command(SND_R_PCM_START, snd.out_stream);\n",
     r#"            snd.out_running = pcm_command(SND_R_PCM_START, snd.out_stream);
#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE
            if (!snd.out_running) {   // OneiOS native: a START the device may have taken late: stop it and start again, once
                pcm_command(SND_R_PCM_STOP, snd.out_stream);
                snd.out_running = pcm_command(SND_R_PCM_START, snd.out_stream);
                printf("droidvm-snd: the speaker's START failed once; again: %s\n", snd.out_running ? "running" : "refused");
            }
#endif
"#),
    // No microphone unless asked: crosvm's AAudio backend opens the phone's input at its PREPARE, which with One UI stopped
    // waits 10 s (take7), and nothing in the VM records yet.
    ("    if (snd.in_stream >= 0 && !set_params(snd.in_stream, snd.in_channels)) snd.in_stream = -1;\n",
     r#"#if defined(XNU_ONEIOS_NATIVE) && XNU_ONEIOS_NATIVE
    {   // OneiOS native: the microphone only with oneios-audio-mic=1 (crosvm opens the phone's input at PREPARE: 10 s, take7)
        uint32_t mic = 0;
        PE_parse_boot_argn("oneios-audio-mic", &mic, sizeof(mic));
        if (!mic) snd.in_stream = -1;
    }
#endif
    if (snd.in_stream >= 0 && !set_params(snd.in_stream, snd.in_channels)) snd.in_stream = -1;
"#),
];
