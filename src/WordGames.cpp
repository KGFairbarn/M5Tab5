#include "WordGames.h"

// ======================== WORD LISTS & HINTS =====================
struct WordDef { const char* word; const char* hint; };
const int NUM_WORDS = 50;
const WordDef wordList[NUM_WORDS] = {
    {"always", "At all times; forever."}, {"around", "Located or situated on every side."},
    {"because", "For the reason that."}, {"been", "Past tense of 'be'."},
    {"before", "During the period of time preceding."}, {"best", "Of the highest quality or excellence."},
    {"both", "Two people or things together."}, {"call", "To cry out or shout to someone."},
    {"cold", "Of or at a low temperature."}, {"does", "Perform an action (he/she/it)."},
    {"fast", "Moving or capable of moving at high speed."}, {"first", "Coming before all others in time or order."},
    {"five", "The number after four."}, {"found", "Having been discovered by chance."},
    {"gave", "Handed over to someone."}, {"goes", "Moves from one place to another."},
    {"green", "The color of grass and leaves."}, {"made", "Formed or put together."},
    {"many", "A large number of."}, {"read", "Look at and comprehend written words."},
    {"right", "Morally good, justified, or acceptable."}, {"sing", "Make musical sounds with the voice."},
    {"sleep", "Rest with eyes closed and mind inactive."}, {"tell", "Communicate information to someone."},
    {"their", "Belonging to or associated with people."}, {"these", "Plural of 'this'."},
    {"those", "Plural of 'that'."}, {"upon", "More formal term for 'on'."},
    {"very", "In a high degree."}, {"wash", "Clean with water and soap."},
    {"which", "Asking for information specifying one or more."}, {"write", "Mark letters or numbers on a surface."},
    {"your", "Belonging to or associated with you."}, {"apple", "A round fruit with red or green skin."},
    {"happy", "Feeling or showing pleasure."}, {"little", "Small in size or amount."},
    {"school", "An institution for educating children."}, {"magic", "The power of apparently influencing events."},
    {"wand", "A long, thin stick used by a wizard."}, {"wizard", "A man who has magical powers."},
    {"witch", "A woman who has magical powers."}, {"spell", "Words used as a magical charm."},
    {"potion", "A liquid with healing or magical properties."}, {"broom", "A brush on a long handle, used for flying."},
    {"hogwarts", "A school of witchcraft and wizardry."}, {"muggle", "A person who is not magical."},
    {"snitch", "A tiny golden ball with wings."}, {"sorting", "Organizing into different houses."},
    {"owl", "A bird of prey that delivers mail."}, {"cloak", "A piece of clothing that can make you invisible."}
};

// ======================== WORD STATE =============================
String currentWord = "";
String currentHint = "";
String guessedLetters = "";
String spellingInput = "";
int applesLeft = 7;
bool wordGameOver = false;
bool wordGameWon = false;
bool spellingMemorizePhase = false;
unsigned long spellingTimer = 0;

// ======================== KEYBOARD ENGINE ========================
void drawKeyboard(int startY, bool showSubmit) {
    const char* row1[] = {"Q","W","E","R","T","Y","U","I","O","P"};
    const char* row2[] = {"A","S","D","F","G","H","J","K","L"};
    const char* row3[] = {"Z","X","C","V","B","N","M","DEL"};
    int keyW = 60, keyH = 75, gapX = 10, gapY = 15;
    M5.Display.setFont(&fonts::FreeSansBold18pt7b);
    
    int startX = (sw - (10 * keyW + 9 * gapX)) / 2;
    for(int i=0; i<10; i++) { M5.Display.fillRoundRect(startX + i*(keyW+gapX), startY, keyW, keyH, 8, TFT_WHITE); M5.Display.drawRoundRect(startX + i*(keyW+gapX), startY, keyW, keyH, 8, C_GRAY); M5.Display.setTextColor(0x3186); M5.Display.drawString(row1[i], startX + i*(keyW+gapX) + keyW/2, startY + keyH/2); }
    startX = (sw - (9 * keyW + 8 * gapX)) / 2;
    for(int i=0; i<9; i++) { M5.Display.fillRoundRect(startX + i*(keyW+gapX), startY + keyH + gapY, keyW, keyH, 8, TFT_WHITE); M5.Display.drawRoundRect(startX + i*(keyW+gapX), startY + keyH + gapY, keyW, keyH, 8, C_GRAY); M5.Display.drawString(row2[i], startX + i*(keyW+gapX) + keyW/2, startY + keyH + gapY + keyH/2); }
    int delW = keyW * 2; startX = (sw - (7 * keyW + delW + 7 * gapX)) / 2;
    for(int i=0; i<7; i++) { M5.Display.fillRoundRect(startX + i*(keyW+gapX), startY + 2*(keyH + gapY), keyW, keyH, 8, TFT_WHITE); M5.Display.drawRoundRect(startX + i*(keyW+gapX), startY + 2*(keyH + gapY), keyW, keyH, 8, C_GRAY); M5.Display.drawString(row3[i], startX + i*(keyW+gapX) + keyW/2, startY + 2*(keyH + gapY) + keyH/2); }
    M5.Display.fillRoundRect(startX + 7*(keyW+gapX), startY + 2*(keyH + gapY), delW, keyH, 8, C_RED); M5.Display.setTextColor(TFT_WHITE); M5.Display.drawString("DEL", startX + 7*(keyW+gapX) + delW/2, startY + 2*(keyH + gapY) + keyH/2);
    if (showSubmit) { M5.Display.fillRoundRect(sw*0.2, startY + 3*(keyH + gapY), sw*0.6, keyH+10, 16, C_GREEN); M5.Display.setTextColor(TFT_WHITE); M5.Display.drawString("ENTER", sw/2, startY + 3*(keyH + gapY) + (keyH+10)/2); }
}

char handleKeyboardTouch(int tx, int ty, int startY, bool hasSubmit) {
    const char* row1[] = {"Q","W","E","R","T","Y","U","I","O","P"}; const char* row2[] = {"A","S","D","F","G","H","J","K","L"}; const char* row3[] = {"Z","X","C","V","B","N","M"};
    int keyW = 60, keyH = 75, gapX = 10, gapY = 15;
    
    if (ty >= startY && ty <= startY + keyH) { int startX = (sw - (10 * keyW + 9 * gapX)) / 2; for(int i=0; i<10; i++) if (tx >= startX + i*(keyW+gapX) && tx <= startX + i*(keyW+gapX) + keyW) return row1[i][0]; }
    if (ty >= startY + keyH + gapY && ty <= startY + 2*keyH + gapY) { int startX = (sw - (9 * keyW + 8 * gapX)) / 2; for(int i=0; i<9; i++) if (tx >= startX + i*(keyW+gapX) && tx <= startX + i*(keyW+gapX) + keyW) return row2[i][0]; }
    if (ty >= startY + 2*(keyH + gapY) && ty <= startY + 3*keyH + 2*gapY) { int delW = keyW * 2, startX = (sw - (7 * keyW + delW + 7 * gapX)) / 2; for(int i=0; i<7; i++) if (tx >= startX + i*(keyW+gapX) && tx <= startX + i*(keyW+gapX) + keyW) return row3[i][0]; if (tx >= startX + 7*(keyW+gapX) && tx <= startX + 7*(keyW+gapX) + delW) return '-'; }
    if (hasSubmit && ty >= startY + 3*(keyH + gapY) && ty <= startY + 4*keyH + 3*gapY + 10) { if (tx >= sw*0.2 && tx <= sw*0.8) return '='; }
    return ' ';
}

// ======================== APPLE TREE =============================
void initAppleTree() {
    int idx = random(0, NUM_WORDS); currentWord = String(wordList[idx].word); currentWord.toUpperCase(); currentHint = String(wordList[idx].hint);
    guessedLetters = ""; applesLeft = 7; wordGameOver = false; wordGameWon = false; drawAppleTree();
}

void drawAppleTree() {
    M5.Display.fillScreen(C_BG); M5.Display.fillRoundRect(20, 20, 120, 50, 16, C_GRAY); M5.Display.setTextDatum(middle_center); M5.Display.setFont(&fonts::FreeSansBold12pt7b); M5.Display.setTextColor(0x3186); M5.Display.drawString("< MENU", 80, 45);
    int treeX = sw/2, treeY = 360; M5.Display.fillRect(treeX - 35, treeY - 80, 70, 220, C_BROWN); M5.Display.fillCircle(treeX, treeY - 140, 130, C_TREEG); M5.Display.fillCircle(treeX - 80, treeY - 90, 100, C_TREEG); M5.Display.fillCircle(treeX + 80, treeY - 90, 100, C_TREEG); 
    int ax[] = {-60, 0, 60, -100, 100, -40, 40}, ay[] = {-160, -200, -160, -80, -80, -80, -80};
    for(int i=0; i<7; i++) { if (applesLeft > i) { M5.Display.fillCircle(treeX + ax[i], treeY + ay[i], 20, C_RED); M5.Display.fillRoundRect(treeX + ax[i] - 2, treeY + ay[i] - 26, 4, 12, 2, C_BROWN); } }
    M5.Display.setFont(&fonts::FreeSansBold24pt7b);
    int letterW = 50, gap = 10, len = currentWord.length(), startX = (sw - (len * letterW + (len-1) * gap)) / 2; bool allGuessed = true;
    for(int i=0; i<len; i++) { char c = currentWord.charAt(i); int lx = startX + i*(letterW+gap); M5.Display.fillRect(lx, 550, letterW, 6, 0x3186); if (guessedLetters.indexOf(c) >= 0 || wordGameOver) { M5.Display.setTextColor((guessedLetters.indexOf(c) < 0 && wordGameOver) ? C_RED : 0x3186); M5.Display.drawString(String(c), lx + letterW/2, 520); } else allGuessed = false; }
    M5.Display.fillRoundRect(sw*0.08, 610, sw*0.84, 75, 12, TFT_WHITE); M5.Display.drawRoundRect(sw*0.08, 610, sw*0.84, 75, 12, C_PURPLE); M5.Display.setFont(&fonts::FreeSansBold12pt7b); M5.Display.setTextColor(C_PURPLE); M5.Display.drawString("HINT: " + currentHint, sw/2, 647);
    if (allGuessed && !wordGameOver) { wordGameOver = true; wordGameWon = true; playTone(880, 150); delay(150); playTone(1175, 300); } else if (applesLeft == 0 && !wordGameOver) { wordGameOver = true; wordGameWon = false; playTone(220, 400); }
    if (wordGameOver) { M5.Display.fillRoundRect(sw*0.2, 700, sw*0.6, 120, 24, wordGameWon ? C_GREEN : C_RED); M5.Display.setTextColor(TFT_WHITE); M5.Display.drawString(wordGameWon ? "YOU WIN!" : "GAME OVER", sw/2, 740); M5.Display.setFont(&fonts::FreeSansBold18pt7b); M5.Display.drawString("Tap to Play Again", sw/2, 790); } else drawKeyboard(700, false);
}

void handleAppleTreeTouch(int tx, int ty) {
    if (tx >= 20 && tx <= 140 && ty >= 20 && ty <= 70) { playTone(440, 60); currentState = STATE_WORD_MENU; drawWordMenu(); return; }
    if (wordGameOver) { if (tx >= sw*0.2 && tx <= sw*0.8 && ty >= 700 && ty <= 820) { playTone(660, 60); initAppleTree(); } return; }
    char letter = handleKeyboardTouch(tx, ty, 700, false);
    if (letter >= 'A' && letter <= 'Z') {
        if (guessedLetters.indexOf(letter) < 0) {
            guessedLetters += letter;
            if (currentWord.indexOf(letter) < 0) { applesLeft--; playTone(220, 150); } else playTone(880, 100); 
            drawAppleTree();
        }
    }
}

// ======================== SPELLING BEE ===========================
void initSpelling() { int idx = random(0, NUM_WORDS); currentWord = String(wordList[idx].word); currentWord.toUpperCase(); spellingInput = ""; wordGameOver = false; spellingMemorizePhase = true; spellingTimer = millis(); playTone(523, 200); delay(200); playTone(659, 200); drawSpelling(); }

void drawSpelling() {
    M5.Display.fillScreen(C_BG); M5.Display.fillRoundRect(20, 20, 120, 50, 16, C_GRAY); M5.Display.setTextDatum(middle_center); M5.Display.setFont(&fonts::FreeSansBold12pt7b); M5.Display.setTextColor(0x3186); M5.Display.drawString("< MENU", 80, 45); M5.Display.setTextColor(C_PURPLE); M5.Display.setFont(&fonts::FreeSansBold24pt7b); M5.Display.drawString("SPELLING BEE", sw/2, 120); M5.Display.fillRoundRect(sw*0.1, 200, sw*0.8, 160, 24, TFT_WHITE); M5.Display.drawRoundRect(sw*0.1, 200, sw*0.8, 160, 24, C_PURPLE);
    if (spellingMemorizePhase) { M5.Display.setFont(&fonts::FreeSans12pt7b); M5.Display.setTextColor(0x3186); M5.Display.drawString("Memorize this word!", sw/2, 230); M5.Display.setFont(&fonts::FreeSansBold24pt7b); M5.Display.setTextColor(C_BLUE); M5.Display.drawString(currentWord, sw/2, 290); M5.Display.fillRoundRect(sw*0.2, 420, sw*0.6, 80, 20, C_ORANGE); M5.Display.setTextColor(TFT_WHITE); M5.Display.setFont(&fonts::FreeSansBold18pt7b); M5.Display.drawString("I'M READY", sw/2, 460); } 
    else { M5.Display.setFont(&fonts::FreeSansBold24pt7b); if (wordGameOver) { M5.Display.setTextColor(wordGameWon ? C_GREEN : C_RED); M5.Display.drawString(spellingInput, sw/2, 280); M5.Display.fillRoundRect(sw*0.2, 420, sw*0.6, 80, 20, C_BLUE); M5.Display.setTextColor(TFT_WHITE); M5.Display.setFont(&fonts::FreeSansBold18pt7b); M5.Display.drawString(wordGameWon ? "CORRECT! NEXT >" : "TRY AGAIN >", sw/2, 460); if (!wordGameWon) { M5.Display.setTextColor(C_RED); M5.Display.setFont(&fonts::FreeSans12pt7b); M5.Display.drawString("It was: " + currentWord, sw/2, 380); } } 
    else { M5.Display.setTextColor(0x3186); String displayStr = spellingInput; if (millis() % 1000 > 500) displayStr += "_"; M5.Display.drawString(displayStr, sw/2, 280); drawKeyboard(520, true); } }
}

void handleSpellingTouch(int tx, int ty) {
    if (tx >= 20 && tx <= 140 && ty >= 20 && ty <= 70) { playTone(440, 60); currentState = STATE_WORD_MENU; drawWordMenu(); return; }
    if (spellingMemorizePhase) { if (tx >= sw*0.2 && tx <= sw*0.8 && ty >= 420 && ty <= 500) { spellingMemorizePhase = false; playTone(660, 60); drawSpelling(); } return; }
    if (wordGameOver) { if (tx >= sw*0.2 && tx <= sw*0.8 && ty >= 420 && ty <= 500) { playTone(660, 60); if(wordGameWon) initSpelling(); else { wordGameOver=false; spellingInput=""; drawSpelling(); } } return; }
    
    char letter = handleKeyboardTouch(tx, ty, 520, true);
    if (letter >= 'A' && letter <= 'Z') { if (spellingInput.length() < currentWord.length() + 3) { spellingInput += letter; playTone(523, 40); drawSpelling(); } } 
    else if (letter == '-') { if (spellingInput.length() > 0) { spellingInput = spellingInput.substring(0, spellingInput.length() - 1); playTone(440, 40); drawSpelling(); } } 
    else if (letter == '=') { if (spellingInput.length() > 0) { wordGameOver = true; if (spellingInput == currentWord) { wordGameWon = true; playTone(880, 150); delay(150); playTone(1175, 300); } else { wordGameWon = false; playTone(220, 300); } drawSpelling(); } }
}

void loopWordGames() {
    if (currentState == STATE_WORD_SPELLING && spellingMemorizePhase) {
        if (millis() - spellingTimer > 500) { if (millis() - spellingTimer > (unsigned long)spellShowTime) { spellingMemorizePhase = false; playTone(440, 150); drawSpelling(); } }
    }
    if (currentState == STATE_WORD_SPELLING && !spellingMemorizePhase && !wordGameOver) {
        if (millis() % 1000 == 0 || millis() % 1000 == 500) drawSpelling();
    }
}