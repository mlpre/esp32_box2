# Game media sources

## Back-facing character frames

- Source: https://github.com/hrhello/cxk-ball
- Files: `images/paddle_1.png`, `images/paddle_2.png`, copied as `back_left.png`, `back_right.png`.
- Original 80 × 120 transparent PNGs are preserved. `tools/generate_game_sprites.ps1` packs their original ARGB pixels; display scaling happens at runtime.
- Repository license copied in `cxk-ball-LICENSE.txt`. This records the repository's license, not a separate assertion about underlying footage rights.

## Recorded background and voice

- Source: https://github.com/dreamhunter2333/ikun-whacamole
- Commit: `3992ccdc7df6816b3f84bd04796b35607fef1fe6`
- Background: `resources/audios/bgm.mp3` (about 59.43 seconds).
- Voice: `resources/audios/niganma.mp3` (3 seconds).
- These are existing meme-game recordings, not generated MIDI or cloned vocals. They are not presented as a verified studio master or complete song.
- Converted using FFmpeg to signed 16-bit little-endian, mono, 24 kHz PCM. Background gain is +8 dB (source peak -9.6 dB), with a brief fade at the loop boundary. No network download happens on the board.
- `bgm.pcm`: only source 27.50-31.78 seconds, a **4.28-second chorus hook**, loops continuously. The introduction, long instrumental and closing speech are excluded.
- `voice.pcm`: only source 0.00-1.20 seconds, the short spoken phrase; plays over lowered background after every fifth basket.
- Both excerpts have short edge fades to prevent clicks. Regenerate with `tools/import_game_audio.ps1`.
