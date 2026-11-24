#include "DiceGame.h"

int currentRollCount = 0;
int diceValues[6];
int diceSum = 0;
unsigned long rollStartTime = 0;
unsigned long totalTime = 0;
bool isShakePhase = true;
bool rollResolved = false;
bool diceGameOver = false;
unsigned long lastTimerUpdate = 0;

void drawDice(int x, int y, int size, int val) {
    M5.Display.fillRoundRect(x, y, size, size, size/6, TFT_WHITE);
    M5.Display.drawRoundRect(x, y, size, size, size/6, C_GRAY);
    int c = x + size/2, q1 = x + size*0.25, q3 = x + size*0.75;
    int cy = y + size/2, q1y = y + size*0.25, q3y = y + size*0.75;
    int d = size/10;
    M5.Display.setColor(TFT_BLACK);
    if(val==1) { M5.Display.fillCircle(c,cy,d); }
    else if(val==2) { M5.Display.fillCircle(q1,q1y,d); M5.Display.fillCircle(q3,q3y,d); }
    else if(val==3) { M5.Display.fillCircle(q1,q1y,d); M5.Display.fillCircle(c,cy,d); M5.Display.fillCircle(q3,q3y,d); }
    else if(val==4) { M5.Display.fillCircle(q1,q1y,d); M5.Display.fillCircle(q3,q1y,d); M5.Display.fillCircle(q1,q3y,d); M5.Display.fillCircle(q3,q3y,d); }
    else if(val==5) { M5.Display.fillCircle(q1,q1y,d); M5.Display.fillCircle(q3,q1y,d); M5.Display.fillCircle(c,cy,d); M5.Display.fillCircle(q1,q3y,d); M5.Display.fillCircle(q3,q3y,d); }
    else if(val==6) { M5.Display.fillCircle(q1,q1y,d); M5.Display.fillCircle(q3,q1y,d); M5.Display.fillCircle(q1,cy,d); M5.Display.fillCircle(q3,cy,d); M5.Display.fillCircle(q1,q3y,d); M5.Display.fillCircle(q3,q3y,d); }
}

void nextDiceRoll() {
    currentRollCount++;
    isShakePhase = true; rollResolved = false; mathManualInput = ""; diceSum = 0;
    if (currentRollCount > numRolls) { diceGameOver = true; drawDiceScreen(); return; }
    drawDiceScreen();
}

void initDiceGame() {
    currentRollCount = 0; totalTime = 0; diceGameOver = false;
    nextDiceRoll();
}

void drawDiceScreen() {
    M5.Display.fillScreen(C_BG);
    M5.Display.fillRoundRect(24, 24, 140, 64, 16, C_GRAY); M5.Display.setTextDatum(middle_center); M5.Display.setFont(&fonts::FreeSansBold12pt7b); M5.Display.setTextColor(0x3186); M5.Display.drawString("< MENU", 94, 56);
    M5.Display.setFont(&fonts::FreeSansBold18pt7b); M5.Display.setTextColor(C_RED); 
    M5.Display.drawString("Roll: " + String(min(currentRollCount, numRolls)) + "/" + String(numRolls), sw - 100, 56);

    if (diceGameOver) {
        float avg = (float)totalTime / (numRolls * 1000.0);
        M5.Display.fillRoundRect(sw*0.1, 200, sw*0.8, 400, 24, TFT_WHITE); M5.Display.drawRoundRect(sw*0.1, 200, sw*0.8, 400, 24, C_RED);
        M5.Display.setTextColor(C_RED); M5.Display.setFont(&fonts::FreeSansBold24pt7b); M5.Display.drawString("GAME OVER", sw/2, 280);
        M5.Display.setTextColor(0x3186); M5.Display.setFont(&fonts::FreeSansBold18pt7b); M5.Display.drawString("Average Counting Time:", sw/2, 380);
        M5.Display.setTextColor(C_BLUE); M5.Display.setFont(&fonts::FreeSansBold24pt7b); M5.Display.drawString(String(avg, 2) + " sec", sw/2, 450);
        M5.Display.fillRoundRect(sw*0.2, 700, sw*0.6, 120, 24, C_GREEN); M5.Display.setTextColor(TFT_WHITE); M5.Display.drawString("PLAY AGAIN", sw/2, 760);
        return;
    }

    if (isShakePhase) {
        M5.Display.setTextColor(C_ORANGE); M5.Display.setFont(&fonts::FreeSansBold24pt7b);
        M5.Display.drawString("SHAKE THE TABLET", sw/2, 300);
        M5.Display.drawString("TO ROLL THE DICE!", sw/2, 360);
    } else {
        int dSize = (numDice > 4) ? 100 : 130;
        int gap = 20;
        int cols = (numDice > 3) ? 3 : numDice;
        int rows = (numDice > 3) ? 2 : 1;
        int startX = (sw - (cols * dSize + (cols-1) * gap)) / 2;
        int startY = 160;

        for (int i=0; i<numDice; i++) {
            int r = i / 3, c = i % 3;
            drawDice(startX + c*(dSize+gap), startY + r*(dSize+gap), dSize, diceValues[i]);
        }
        M5.Display.fillRoundRect(sw*0.3, 440, sw*0.4, 70, 16, C_GRAY);
        uint16_t eqColor = rollResolved ? (mathManualInput.toInt() == diceSum ? C_GREEN : C_RED) : 0x3186;
        String eq = "Sum: " + (mathManualInput == "" ? "?" : mathManualInput);
        M5.Display.setTextColor(eqColor); M5.Display.setFont(&fonts::FreeSansBold24pt7b); M5.Display.drawString(eq, sw/2, 570);

        if (rollResolved) {
            M5.Display.fillRoundRect(sw*0.2, 850, sw*0.6, 100, 24, C_GREEN); M5.Display.setTextColor(TFT_WHITE); M5.Display.drawString("NEXT ROLL >", sw/2, 900);
        } else {
            const char* keys[12] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "DEL", "0", "GO"};
            for(int i = 0; i < 12; i++) {
                int col = i % 3, row = i / 3, bx = 100 + col * 180, by = 640 + row * 100;
                uint16_t color = C_BLUE; if (i == 9) color = C_RED; if (i == 11) color = C_GREEN;
                M5.Display.fillRoundRect(bx, by, 160, 85, 16, color); M5.Display.setTextColor(TFT_WHITE); M5.Display.setFont(&fonts::FreeSansBold24pt7b); M5.Display.drawString(keys[i], bx + 80, by + 42);
            }
        }
    }
}

void handleDiceTouch(int tx, int ty) {
    if (tx >= 24 && tx <= 164 && ty >= 24 && ty <= 88) { playTone(440, 60); currentState = STATE_MATH_MENU; drawMathMenu(); return; }
    if (diceGameOver && tx >= sw*0.2 && tx <= sw*0.8 && ty >= 700 && ty <= 820) { playTone(660, 60); initDiceGame(); return; }
    
    if (!isShakePhase && !rollResolved) {
        for(int i = 0; i < 12; i++) {
            int col = i % 3, row = i / 3, bx = 100 + col * 180, by = 640 + row * 100;
            if (tx >= bx && tx <= bx + 160 && ty >= by && ty <= by + 85) {
                if (i == 9) { if (mathManualInput.length() > 0) { mathManualInput = mathManualInput.substring(0, mathManualInput.length() - 1); playTone(440, 40); drawDiceScreen(); } } 
                else if (i == 11) { 
                    if (mathManualInput.length() > 0) { 
                        if (mathManualInput.toInt() == diceSum) { totalTime += (millis() - rollStartTime); rollResolved = true; playTone(880, 120); drawDiceScreen(); } 
                        else { mathManualInput = ""; playTone(220, 250); drawDiceScreen(); }
                    } 
                } 
                else { if (mathManualInput.length() < 3) { String val = (i == 10) ? "0" : String(i + 1); mathManualInput += val; playTone(523, 40); drawDiceScreen(); } }
                return;
            }
        }
    }
    else if (rollResolved && tx >= sw*0.2 && tx <= sw*0.8 && ty >= 850 && ty <= 950) {
        playTone(660, 60); nextDiceRoll();
    }
}

void loopDiceGame() {
    if (currentState == STATE_MATH_DICE && !diceGameOver) {
        if (isShakePhase) {
            float ax, ay, az;
            M5.Imu.getAccelData(&ax, &ay, &az);
            float gForce = sqrt(ax*ax + ay*ay + az*az);
            if (gForce > 2.0) { // Shake threshold
                diceSum = 0;
                for (int i=0; i<numDice; i++) { diceValues[i] = random(1, 7); diceSum += diceValues[i]; }
                playTone(523, 50); delay(50); playTone(587, 50); delay(50); playTone(659, 100);
                isShakePhase = false; rollStartTime = millis(); drawDiceScreen();
            }
        } else if (!rollResolved) {
            if (millis() - lastTimerUpdate > 100) {
                lastTimerUpdate = millis();
                float elapsed = (millis() - rollStartTime) / 1000.0;
                M5.Display.fillRoundRect(sw*0.3, 440, sw*0.4, 70, 16, C_GRAY);
                M5.Display.setTextColor(C_BLUE); M5.Display.setFont(&fonts::FreeSansBold24pt7b);
                M5.Display.drawString(String(elapsed, 1) + " sec", sw/2, 475);
            }
        }
    }
}