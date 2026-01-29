#pragma once

void playBeep();
void playLongBeep();
void playStartMelody();
void playShutdownMelody();
void playGPSEnableBeep();
void playGPSDisableBeep();
void playComboTune();
void play4ClickDown();
void play4ClickUp();
void playBoop();
void playChirp();
void playClick();
void playLongPressLeadUp();
bool playNextLeadUpNote();  // Play the next note in the lead-up sequence
void resetLeadUpSequence(); // Reset the lead-up sequence to start from beginning

/** Two-note "ta-daa" confirmation (1000 Hz 100 ms, pause 100 ms, 1800 Hz 180 ms). Caller must gate by buzzer config. */
void playSosAckConfirmationTones();