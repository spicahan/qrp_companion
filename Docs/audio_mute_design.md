Rationale: QMX Companion in the dbe8b1d2 rev sometimes (I have'nt figured out exactly what changed) outputs the audio for the entire SPAN as opposed to just the audio for the CW filtered pass band.  Zhenxing, N6HAN, the originator,  realized that it was easier to use the QRPLabs QMX built-in filters than to duplicate that audio filtering in this code.  However, the volume of the TAB5 audio is distracting when listenting to the QMX ... even with headphones.   This effort is to add an audio mute feaure to the code. THis is also a training task for me as I have not used coding agents before.  THis seems like a good application to learn this work flow.

Objective: Add a software mute feature to the audio subsystem.  THis should be selected via the UI as a button.

Requirements:
- Implement mute inside audioOutputWrite()
- Do NOT stop/start M5.Speaker
- Do NOT alter buffering architecture
- Add:
    static bool s_audio_muted
    void audioSetMuted(bool)
    bool audioIsMuted()
- Expose declarations in pal.h
- Mute should zero outgoing samples before int16 conversion
- Add a button to the UI at the right of the exting "DR" button at the display bottom.  It should be labeled "Mute Audio" when the audio is on and "Audio Muted" when the audio is muted.

Patch the existing firmware accordingly.