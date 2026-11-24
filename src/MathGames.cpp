#include "MathGames.h"

// ======================== FLASHCARDS STATE =======================
int fcA = 0, fcB = 0, fcAns = 0, fcScore = 0, fcSelected = -1;
char fcOp = '+';
int fcChoices[4];
bool fcResolved = false;

// ======================== CROSSMATH STATE ========================
const int ROWS = 7, COLS = 9;
enum CellType { CELL_EMPTY, CELL_FIXED, CELL_OP, CELL_SLOT };
struct GridCell { CellType type; int targetVal; int currentVal; char op; int bankIdx; };
GridCell grid[ROWS][COLS];
int bankValues[6];
bool bankUsed[6];
int selectedBankIdx = -1;
bool cmSolved = false;

// ======================== FLASHCARDS LOGIC =======================
void initFlashcards() {
    fcSelected = -1; fcResolved = false; mathManualInput = "";
    if (!useAdd && !useSub && !useMult && !useDiv) useAdd = true;
    int opChoice = -1;
    while(true) { opChoice = random(0, 4); if ((opChoice == 0 && useAdd) || (opChoice == 1 && useSub) || (opChoice == 2 && useMult) || (opChoice == 3 && useDiv)) break; }

    if (opChoice == 0) { fcOp = '+'; fcA = random(1, maxNumLimit); fcB = random(1, maxNumLimit - fcA + 1); fcAns = fcA + fcB; } 
    else if (opChoice == 1) { fcOp = '-'; fcA = random(2, maxNumLimit + 1); fcB = random(1, fcA); fcAns = fcA - fcB; } 
    else if (opChoice == 2) { fcOp = 'x'; fcA = random(1, (maxNumLimit / 2) + 2); if (fcA > maxNumLimit) fcA = maxNumLimit; int maxB = maxNumLimit / fcA; if (maxB < 1) maxB = 1; fcB = random(1, maxB + 1); fcAns = fcA * fcB; } 
    else if (opChoice == 3) { fcOp = '/'; fcB = random(1, (maxNumLimit / 2) + 2); if (fcB > maxNumLimit) fcB = maxNumLimit; int maxAns = maxNumLimit / fcB; if (maxAns < 1) maxAns = 1; fcAns = random(1, maxAns + 1); fcA = fcAns * fcB; }

    int correctPos = random(0, 4);
    for (int i = 0; i < 4; i++) {
        if (i == correctPos) fcChoices[i] = fcAns;
        else {
            int wrong = fcAns; while (wrong == fcAns || wrong < 0) { int offset = random(-5, 6); if (offset == 0) offset = 2; wrong = fcAns + offset; if (wrong > maxNumLimit + 5) wrong = maxNumLimit + 2; }
            for (int j = 0; j < i; j++) if (fcChoices[j] == wrong) wrong = fcAns + i + 5;
            fcChoices[i] = wrong;
        }
    }
    drawFlashcards();
}

void drawFlashcards() {
    M5.Display.fillScreen(C_BG);
    M5.Display.fillRoundRect(24, 24, 140, 64, 16, C_GRAY); M5.Display.setTextDatum(middle_center); M5.Display.setFont(&fonts::FreeSansBold12pt7b); M5.Display.setTextColor(0x3186); M5.Display.drawString("< MENU", 94, 56);
    M5.Display.setFont(&fonts::FreeSansBold18pt7b); M5.Display.setTextColor(0xE580); M5.Display.drawString("Score: " + String(fcScore), sw - 130, 56);
    
    int cardX = sw * 0.08, cardY = sh * 0.14, cardW = sw * 0.84, cardH = sh * 0.32;
    M5.Display.fillRoundRect(cardX + 6, cardY + 8, cardW, cardH, 30, 0xCE79); M5.Display.fillRoundRect(cardX, cardY, cardW, cardH, 30, TFT_WHITE); M5.Display.drawRoundRect(cardX, cardY, cardW, cardH, 30, C_BLUE);

    uint16_t eqColor = 0x18E3;
    if (fcResolved) eqColor = (useMultipleChoice ? (fcChoices[fcSelected] == fcAns) : (mathManualInput.toInt() == fcAns)) ? C_GREEN : C_RED;

    String eq = String(fcA) + " " + String(fcOp) + " " + String(fcB) + " = " + (fcResolved ? String(fcAns) : (useMultipleChoice ? "?" : (mathManualInput == "" ? "?" : mathManualInput)));
    M5.Display.setFont(&fonts::FreeSansBold24pt7b); M5.Display.setTextSize(2); M5.Display.setTextColor(eqColor); M5.Display.drawString(eq, sw / 2, cardY + cardH / 2); M5.Display.setTextSize(1);

    if (useMultipleChoice) {
        int btnW = sw * 0.40, btnH = sh * 0.13;
        for (int i = 0; i < 4; i++) {
            int col = i % 2, row = i / 2, bx = (sw * 0.08) + col * (btnW + sw * 0.04), by = (sh * 0.52) + row * (btnH + sh * 0.03);
            uint16_t fillCol = C_BLUE;
            if (fcSelected == i) fillCol = (fcChoices[i] == fcAns) ? C_GREEN : C_RED; else if (fcResolved && fcChoices[i] == fcAns) fillCol = C_GREEN;
            M5.Display.fillRoundRect(bx, by, btnW, btnH, 22, fillCol); M5.Display.setFont(&fonts::FreeSansBold24pt7b); M5.Display.setTextColor(TFT_WHITE); M5.Display.drawString(String(fcChoices[i]), bx + btnW / 2, by + btnH / 2);
        }
    } else {
        if (!fcResolved) {
            const char* keys[12] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "DEL", "0", "GO"};
            for(int i = 0; i < 12; i++) {
                int col = i % 3, row = i / 3, bx = 100 + col * 180, by = 640 + row * 100;
                uint16_t color = C_BLUE; if (i == 9) color = C_RED; if (i == 11) color = C_GREEN;
                M5.Display.fillRoundRect(bx, by, 160, 85, 16, color); M5.Display.setTextColor(TFT_WHITE); M5.Display.setFont(&fonts::FreeSansBold24pt7b); M5.Display.drawString(keys[i], bx + 80, by + 42);
            }
        }
    }
    if (fcResolved) { M5.Display.fillRoundRect(sw * 0.25, sh * 0.87, sw * 0.50, sh * 0.08, 20, C_ORANGE); M5.Display.setFont(&fonts::FreeSansBold18pt7b); M5.Display.setTextColor(TFT_WHITE); M5.Display.drawString("NEXT CARD >", sw / 2, sh * 0.91); }
}

void handleFlashcardsTouch(int tx, int ty) {
    if (tx >= 24 && tx <= 164 && ty >= 24 && ty <= 88) { playTone(440, 60); currentState = STATE_MATH_MENU; drawMathMenu(); return; }
    if (fcResolved && tx >= sw * 0.25 && tx <= sw * 0.75 && ty >= sh * 0.85 && ty <= sh * 0.96) { playTone(660, 60); initFlashcards(); return; }
    if (!fcResolved) {
        if (useMultipleChoice) {
            int btnW = sw * 0.40, btnH = sh * 0.13;
            for (int i = 0; i < 4; i++) {
                int col = i % 2, row = i / 2, bx = (sw * 0.08) + col * (btnW + sw * 0.04), by = (sh * 0.52) + row * (btnH + sh * 0.03);
                if (tx >= bx && tx <= bx + btnW && ty >= by && ty <= by + btnH) {
                    fcSelected = i; fcResolved = true; if (fcChoices[i] == fcAns) { fcScore++; playTone(880, 120); } else playTone(220, 250);
                    drawFlashcards(); return;
                }
            }
        } else {
            for(int i = 0; i < 12; i++) {
                int col = i % 3, row = i / 3, bx = 100 + col * 180, by = 640 + row * 100;
                if (tx >= bx && tx <= bx + 160 && ty >= by && ty <= by + 85) {
                    if (i == 9) { if (mathManualInput.length() > 0) { mathManualInput = mathManualInput.substring(0, mathManualInput.length() - 1); playTone(440, 40); drawFlashcards(); } } 
                    else if (i == 11) { if (mathManualInput.length() > 0) { fcResolved = true; if (mathManualInput.toInt() == fcAns) { fcScore++; playTone(880, 120); } else playTone(220, 250); drawFlashcards(); } } 
                    else { if (mathManualInput.length() < 4) { String val = (i == 10) ? "0" : String(i + 1); mathManualInput += val; playTone(523, 40); drawFlashcards(); } }
                    return;
                }
            }
        }
    }
}

// ======================== CROSSMATH LOGIC ========================
bool evalLine(int v1, char op, int v2, int res) { if (v1 < 0 || v2 < 0 || res < 0) return false; return (op == '+') ? (v1 + v2 == res) : (v1 - v2 == res); }
int getVal(int r, int c) { return grid[r][c].currentVal; }
void checkCrossmathWin() {
    for (int r = 0; r < ROWS; r++) for (int c = 0; c < COLS; c++) if (grid[r][c].type == CELL_SLOT && grid[r][c].currentVal == -1) return;
    if (evalLine(getVal(0,0), '+', getVal(0,2), getVal(0,4)) && evalLine(getVal(0,0), '+', getVal(2,0), getVal(4,0)) && evalLine(getVal(0,2), '+', getVal(2,2), getVal(4,2)) && evalLine(getVal(4,0), '-', getVal(4,2), getVal(4,4)) && evalLine(getVal(2,4), '-', getVal(4,4), getVal(6,4)) && evalLine(getVal(2,4), '+', getVal(2,6), getVal(2,8))) {
        cmSolved = true; playTone(880, 120); delay(130); playTone(1175, 250);
    } else playTone(220, 200);
}

void initCrossmath() {
    for (int r = 0; r < ROWS; r++) for (int c = 0; c < COLS; c++) grid[r][c] = {CELL_EMPTY, 0, -1, ' ', -1};
    selectedBankIdx = -1; cmSolved = false;

    int baseLim = maxNumLimit / 2; if (baseLim < 4) baseLim = 4; if (baseLim > 12) baseLim = 12;
    int B = random(2, baseLim), E = random(2, baseLim), J = B + E, K = random(2, baseLim), I = J + K;
    int A = random(2, I - 1); if (A < 1) A = 1;
    int D = I - A, C = A + B, L = random(3, baseLim + 2), F = K + L, G = random(2, baseLim), H = F + G;
    int slotIdx = 0;

    grid[0][0] = {CELL_SLOT, A, -1, ' ', -1}; bankValues[slotIdx] = A; bankUsed[slotIdx] = false; slotIdx++;
    grid[0][1] = {CELL_OP, 0, -1, '+', -1}; grid[0][2] = {CELL_FIXED, B, B, ' ', -1}; grid[0][3] = {CELL_OP, 0, -1, '=', -1}; grid[0][4] = {CELL_FIXED, C, C, ' ', -1};
    grid[1][0] = {CELL_OP, 0, -1, '+', -1}; grid[1][2] = {CELL_OP, 0, -1, '+', -1};
    grid[2][0] = {CELL_SLOT, D, -1, ' ', -1}; bankValues[slotIdx] = D; bankUsed[slotIdx] = false; slotIdx++;
    grid[2][2] = {CELL_SLOT, E, -1, ' ', -1}; bankValues[slotIdx] = E; bankUsed[slotIdx] = false; slotIdx++;
    grid[2][4] = {CELL_SLOT, F, -1, ' ', -1}; bankValues[slotIdx] = F; bankUsed[slotIdx] = false; slotIdx++;
    grid[2][5] = {CELL_OP, 0, -1, '+', -1}; grid[2][6] = {CELL_FIXED, G, G, ' ', -1}; grid[2][7] = {CELL_OP, 0, -1, '=', -1}; 
    grid[2][8] = {CELL_SLOT, H, -1, ' ', -1}; bankValues[slotIdx] = H; bankUsed[slotIdx] = false; slotIdx++;
    grid[3][0] = {CELL_OP, 0, -1, '=', -1}; grid[3][2] = {CELL_OP, 0, -1, '=', -1}; grid[3][4] = {CELL_OP, 0, -1, '-', -1};
    grid[4][0] = {CELL_FIXED, I, I, ' ', -1}; grid[4][1] = {CELL_OP, 0, -1, '-', -1}; 
    grid[4][2] = {CELL_SLOT, J, -1, ' ', -1}; bankValues[slotIdx] = J; bankUsed[slotIdx] = false; slotIdx++;
    grid[4][3] = {CELL_OP, 0, -1, '=', -1}; grid[4][4] = {CELL_FIXED, K, K, ' ', -1};
    grid[5][4] = {CELL_OP, 0, -1, '=', -1}; grid[6][4] = {CELL_FIXED, L, L, ' ', -1};

    for (int i = 5; i > 0; i--) { int j = random(0, i + 1); int temp = bankValues[i]; bankValues[i] = bankValues[j]; bankValues[j] = temp; }
    drawCrossmath();
}

void drawCrossmath() {
    M5.Display.fillScreen(C_BG);
    M5.Display.fillRoundRect(20, 20, 130, 56, 14, C_GRAY); M5.Display.setTextDatum(middle_center); M5.Display.setFont(&fonts::FreeSansBold12pt7b); M5.Display.setTextColor(0x3186); M5.Display.drawString("< MENU", 85, 48);
    M5.Display.fillRoundRect(170, 20, 140, 56, 14, C_GRAY); M5.Display.drawString("NEW", 240, 48);
    M5.Display.fillRoundRect(sw - 160, 20, 140, 56, 14, 0xF680); M5.Display.setTextColor(0x0000); M5.Display.drawString("HINT", sw - 90, 48);

    int cellS = (sw - 36) / COLS, gridStartX = (sw - (cellS * COLS)) / 2, gridStartY = 120;
    M5.Display.setFont(&fonts::FreeSansBold18pt7b);
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            if (grid[r][c].type == CELL_EMPTY) continue;
            int x = gridStartX + c * cellS, y = gridStartY + r * cellS;
            uint16_t bgCol = 0xF6F6, borderCol = 0xC5B4, txtCol = 0x1082;
            if (grid[r][c].type == CELL_SLOT) { if (grid[r][c].currentVal == -1) bgCol = 0xFFDF; else { bgCol = cmSolved ? 0xB7F0 : 0xE7FF; txtCol = 0x04A3; } }
            M5.Display.fillRect(x, y, cellS - 2, cellS - 2, bgCol); M5.Display.drawRect(x, y, cellS - 2, cellS - 2, borderCol);
            if (grid[r][c].type == CELL_FIXED || grid[r][c].type == CELL_OP || (grid[r][c].type == CELL_SLOT && grid[r][c].currentVal != -1)) {
                M5.Display.setTextColor(txtCol); String valStr = (grid[r][c].type == CELL_OP) ? String(grid[r][c].op) : String(grid[r][c].targetVal);
                if (grid[r][c].type == CELL_SLOT) valStr = String(grid[r][c].currentVal); M5.Display.drawString(valStr, x + cellS / 2, y + cellS / 2);
            }
        }
    }
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    if (cmSolved) { M5.Display.fillRoundRect(sw * 0.15, gridStartY + ROWS * cellS + 20, sw * 0.70, 56, 16, C_GREEN); M5.Display.setTextColor(TFT_WHITE); M5.Display.drawString("AWESOME JOB! PUZZLE SOLVED!", sw / 2, gridStartY + ROWS * cellS + 48); } 
    else { M5.Display.setTextColor(0x528A); M5.Display.drawString("Tap a green tile below, then tap an empty square!", sw / 2, gridStartY + ROWS * cellS + 45); }

    int bankY = sh - 340; M5.Display.fillRoundRect(20, bankY, sw - 40, 310, 24, TFT_WHITE); M5.Display.drawRoundRect(20, bankY, sw - 40, 310, 24, C_GRAY);
    int tileW = 110, tileH = 110, gapX = 24, gapY = 24, startBankX = (sw - (5 * tileW + 4 * gapX)) / 2, startBankY = bankY + 30;
    M5.Display.setFont(&fonts::FreeSansBold24pt7b);
    for (int i = 0; i < 6; i++) {
        if (bankUsed[i]) continue;
        int col = i % 5, row = i / 5, tx = startBankX + col * (tileW + gapX), ty = startBankY + row * (tileH + gapY);
        uint16_t tBg = (selectedBankIdx == i) ? 0xB7F0 : 0xE7FF, tBorder = (selectedBankIdx == i) ? 0x04A3 : 0x9614;
        M5.Display.fillRoundRect(tx, ty, tileW, tileH, 12, tBg); M5.Display.drawRoundRect(tx, ty, tileW, tileH, 12, tBorder);
        M5.Display.setTextColor(0x04A3); M5.Display.drawString(String(bankValues[i]), tx + tileW / 2, ty + tileH / 2);
    }
}

void handleCrossmathTouch(int tx, int ty) {
    if (tx >= 20 && tx <= 150 && ty >= 20 && ty <= 76) { playTone(440, 60); currentState = STATE_MATH_MENU; drawMathMenu(); return; }
    if (tx >= 170 && tx <= 310 && ty >= 20 && ty <= 76) { playTone(660, 60); initCrossmath(); return; }
    if (tx >= sw - 160 && tx <= sw - 20 && ty >= 20 && ty <= 76 && !cmSolved) {
        for (int r = 0; r < ROWS; r++) {
            for (int c = 0; c < COLS; c++) {
                if (grid[r][c].type == CELL_SLOT && grid[r][c].currentVal != grid[r][c].targetVal) {
                    if (grid[r][c].bankIdx != -1) bankUsed[grid[r][c].bankIdx] = false;
                    for (int b = 0; b < 6; b++) {
                        if (!bankUsed[b] && bankValues[b] == grid[r][c].targetVal) {
                            bankUsed[b] = true; grid[r][c].currentVal = bankValues[b]; grid[r][c].bankIdx = b; selectedBankIdx = -1;
                            playTone(784, 80); checkCrossmathWin(); drawCrossmath(); return;
                        }
                    }
                }
            }
        }
    }
    int bankY = sh - 340, tileW = 110, tileH = 110, gapX = 24, gapY = 24, startBankX = (sw - (5 * tileW + 4 * gapX)) / 2, startBankY = bankY + 30;
    for (int i = 0; i < 6; i++) {
        if (bankUsed[i]) continue;
        int col = i % 5, row = i / 5, bx = startBankX + col * (tileW + gapX), by = startBankY + row * (tileH + gapY);
        if (tx >= bx && tx <= bx + tileW && ty >= by && ty <= by + tileH) { playTone(523, 40); selectedBankIdx = (selectedBankIdx == i) ? -1 : i; drawCrossmath(); return; }
    }
    int cellS = (sw - 36) / COLS, gridStartX = (sw - (cellS * COLS)) / 2, gridStartY = 120;
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            if (grid[r][c].type != CELL_SLOT) continue;
            int gx = gridStartX + c * cellS, gy = gridStartY + r * cellS;
            if (tx >= gx && tx <= gx + cellS && ty >= gy && ty <= gy + cellS) {
                if (selectedBankIdx != -1) {
                    if (grid[r][c].bankIdx != -1) bankUsed[grid[r][c].bankIdx] = false;
                    grid[r][c].currentVal = bankValues[selectedBankIdx]; grid[r][c].bankIdx = selectedBankIdx; bankUsed[selectedBankIdx] = true; selectedBankIdx = -1;
                    playTone(659, 50); checkCrossmathWin(); drawCrossmath();
                } else if (grid[r][c].bankIdx != -1 && !cmSolved) {
                    bankUsed[grid[r][c].bankIdx] = false; selectedBankIdx = grid[r][c].bankIdx; grid[r][c].currentVal = -1; grid[r][c].bankIdx = -1;
                    playTone(440, 50); drawCrossmath();
                }
                return;
            }
        }
    }
}