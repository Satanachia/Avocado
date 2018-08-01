#include "spu.h"
#include <cstring>
#include <array>
#include "sound/adpcm.h"
#include "system.h"

SPU::SPU() {
    memset(ram, 0, RAM_SIZE);
    audioBufferPos = 0;
}

// Convert normalized float to int16_T
int16_t floatToInt(float val) {
    if (val > 0) return val * static_cast<int16_t>(INT16_MAX);
    else return -val * static_cast<int16_t>(INT16_MIN);
}

// Convert int16_t to normalized float
float intToFloat(int16_t val) {
    if (val > 0) return static_cast<float>(val) / static_cast<float>(INT16_MAX);
    else return -static_cast<float>(val) / static_cast<float>(INT16_MIN);
}

float clamp(float val, float min, float max) {
    if (val > max) return max;
    if (val < min) return min;
    return val;
}

void SPU::step() {
    float sumLeft = 0;
    float sumRight = 0;
    for (int v = 0; v < VOICE_COUNT; v++) {
        Voice& voice = voices[v];

        if (!voice.playing) continue;

        if (voice.decodedSamples.empty()) {
            voice.decodedSamples = ADPCM::decode(&ram[voice.currentAddress._reg * 8], voice.prevSample);
        }

        // Modify volume
        float sample = intToFloat(voice.decodedSamples[(int)voice.subAddress]);

        sumLeft += sample * voice.volume.getLeft();
        sumRight += sample * voice.volume.getRight();

        int prevAddr = voice.subAddress;
        voice.subAddress += (float)std::min((uint16_t)0x1000, voice.sampleRate._reg) / 4096.f;

        if ((int)prevAddr == (int)voice.subAddress) continue;

        if (voice.subAddress >= 28) {
            voice.subAddress -= 28;
            voice.currentAddress._reg += 2;

            voice.decodedSamples.clear();
            continue;
        }

        uint8_t flag = ram[voice.currentAddress._reg * 8 + 1];

        if (flag & 4) {  // Loop start
            voice.repeatAddress._reg = voice.currentAddress._reg;
        }

        if (flag & 1) {  // Loop end
            voice.currentAddress._reg = voice.repeatAddress._reg;
            voice.playing = false;
        }
    }

    sumLeft *= mainVolume.getLeft();
    sumRight *= mainVolume.getRight();
    
    audioBuffer[audioBufferPos] = floatToInt(clamp(sumLeft, -1.f, 1.f));
    audioBuffer[audioBufferPos + 1] = floatToInt(clamp(sumRight, -1.f, 1.f));

    audioBufferPos += 2;
    if (audioBufferPos >= AUDIO_BUFFER_SIZE) {
        audioBufferPos = 0;
        bufferReady = true;
    }
}

uint8_t SPU::readVoice(uint32_t address) const {
    int voice = address / 0x10;
    int reg = address % 0x10;

    switch (reg) {
        case 0:
        case 1:
        case 2:
        case 3: return voices[voice].volume.read(reg);

        case 4:
        case 5: return voices[voice].sampleRate.read(reg - 4);

        case 6:
        case 7: return voices[voice].startAddress.read(reg - 6);

        case 8:
        case 9:
        case 10:
        case 11: return voices[voice].ADSR.read(reg - 8);

        case 12:
        case 13: return voices[voice].ADSRVolume.read(reg - 12);

        case 14:
        case 15: return voices[voice].repeatAddress.read(reg - 14);

        default: return 0;
    }
}

void SPU::writeVoice(uint32_t address, uint8_t data) {
    int voice = address / 0x10;
    int reg = address % 0x10;

    switch (reg) {
        case 0:
        case 1: 
        case 2:
        case 3: voices[voice].volume.write(reg, data); return;

        case 4:
        case 5: voices[voice].sampleRate.write(reg - 4, data); return;

        case 6:
        case 7:
            voices[voice].subAddress = 0.f;
            voices[voice].startAddress.write(reg - 6, data);
            return;

        case 8:
        case 9:
        case 10:
        case 11: voices[voice].ADSR.write(reg - 8, data); return;

        case 12:
        case 13: voices[voice].ADSRVolume.write(reg - 12, data); return;

        case 14:
        case 15: voices[voice].repeatAddress.write(reg - 14, data); return;

        default: return;
    }
}

void SPU::voiceKeyOn(int i) {
    Voice& voice = voices[i];

    voice.ADSRVolume._reg = 0;
    voice.repeatAddress._reg = voice.startAddress._reg;
    voice.currentAddress._reg = voice.startAddress._reg;
    voice.playing = true;
}

void SPU::voiceKeyOff(int i) {
    Voice& voice = voices[i];

    voice.playing = false;
}

uint8_t SPU::read(uint32_t address) {
    address += BASE_ADDRESS;

    if (address >= 0x1f801c00 && address < 0x1f801c00 + 0x10 * VOICE_COUNT) {
        return readVoice(address - 0x1f801c00);
    }

    if (address >= 0x1f801da6 && address <= 0x1f801da7) {  // Data address
        return dataAddress.read(address - 0x1f801da6);
    }

    if (address >= 0x1f801daa && address <= 0x1f801dab) {  // SPUCNT
        // printf("SPUCNT READ 0x%04x\n", SPUCNT._reg);
        return control._byte[address - 0x1f801daa];
    }

    if (address >= 0x1f801dac && address <= 0x1f801dad) {  // Data Transfer Control
        return dataTransferControl._byte[address - 0x1f801dac];
    }

    if (address >= 0x1f801dae && address <= 0x1f801daf) {  // SPUSTAT
        SPUSTAT._reg = control._reg & 0x3F;
        // printf("SPUSTAT READ 0x%04x\n", SPUSTAT._reg);
        return SPUSTAT.read(address - 0x1f801dae);
    }

    //    printf("UNHANDLED SPU READ AT 0x%08x\n", address);

    return 0;
}

void SPU::write(uint32_t address, uint8_t data) {
    address += BASE_ADDRESS;

    if (address >= 0x1f801c00 && address < 0x1f801c00 + 0x10 * VOICE_COUNT) {
        writeVoice(address - 0x1f801c00, data);
        return;
    }

    if (address >= 0x1f801d80 && address <= 0x1f801d83) {  // Main Volume L/R
        mainVolume.write(address - 0x1f801d80, data);
        return;
    }

    if (address >= 0x1f801d84 && address <= 0x1f801d87) {  // Reverb Volume L/R
        reverbVolume.write(address - 0x1f801d84, data);
        return;
    }

    if (address >= 0x1f801d88 && address <= 0x1f801d8b) {  // Voices Key On
        static Reg32 keyOn;

        keyOn.write(address - 0x1f801d88, data);
        if (address == 0x1f801d8b) {
            for (int i = 0; i < VOICE_COUNT; i++) {
                if (keyOn.getBit(i)) voiceKeyOn(i);
            }
        }
        return;
    }

    if (address >= 0x1f801d8c && address <= 0x1f801d8f) {  // Voices Key Off
        static Reg32 keyOff;

        keyOff.write(address - 0x1f801d8c, data);
        if (address == 0x1f801d8c) {
            for (int i = 0; i < VOICE_COUNT; i++) {
                if (keyOff.getBit(i)) voiceKeyOff(i);
            }
        }
        return;
    }

    if (address >= 0x1f801d98 && address <= 0x1f801d9b) {  // Voices Channel Reverb mode
        voiceChannelReverbMode.write(address - 0x1f801d98, data);
        return;
    }

    if (address >= 0x1f801da4 && address <= 0x1f801da5) {  // IRQ Address
        irqAddress.write(address - 0x1f801da4, data);
        printf("IRQ ADDRESS: 0x%08x\n", irqAddress._reg * 8);
        return;
    }

    if (address >= 0x1f801da6 && address <= 0x1f801da7) {  // Data address
        dataAddress.write(address - 0x1f801da6, data);
        currentDataAddress = dataAddress._reg * 8;
        return;
    }

    if (address >= 0x1f801da8 && address <= 0x1f801da9) {  // Data FIFO
        if (currentDataAddress >= RAM_SIZE) {
            currentDataAddress %= RAM_SIZE;
        }
        ram[currentDataAddress++] = data;
        return;
    }

    if (address >= 0x1f801daa && address <= 0x1f801dab) {  // SPUCNT
        control._byte[address - 0x1f801daa] = data;
        return;
    }

    if (address >= 0x1f801dac && address <= 0x1f801dad) {  // Data Transfer Control
        dataTransferControl._byte[address - 0x1f801dac] = data;
        return;
    }

    if (address >= 0x1f801dae && address <= 0x1f801daf) {  // SPUSTAT
        SPUSTAT.write(address - 0x1f801dae, data);
        return;
    }

    if (address >= 0x1f801db0 && address <= 0x1f801db3) {  // CD Volume L/R
        cdVolume.write(address - 0x1f801db0, data);
        return;
    }

    if (address >= 0x1F801DB4 && address <= 0x1F801DB3) {  // External input Volume L/R
        extVolume.write(address - 0x1F801DB4, data);
        return;
    }

    //    printf("UNHANDLED SPU WRITE AT 0x%08x: 0x%02x\n", address, data);
}

void SPU::dumpRam() {
    std::vector<uint8_t> ram;
    ram.assign(this->ram, this->ram + RAM_SIZE - 1);
    putFileContents("spu.bin", ram);
}