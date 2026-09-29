FRANKIE'S KARAOKE STUDIO - WINDOWS TEST COPY
=============================================

This copy installs nothing: the program is in the "Program" folder. It keeps
its own settings, playlists and song list in

    %LOCALAPPDATA%\Granda\FrankiesKaraokeStudio

(paste that into the File Explorer address bar to open it) and it never
writes anything into a music folder.

DO NOT CONNECT THE REAL KARAOKE DRIVE UNTIL PART A HAS PASSED.
Before unzipping: right-click the downloaded ZIP > Properties > tick
"Unblock" > OK. Then unzip it somewhere simple, e.g. C:\FKS-Test.


PART A - SAFETY FIRST (about 20 minutes)
----------------------------------------
1. Double-click "Check Installation.cmd". You should hear a short beep.
   After about 10 seconds Notepad shows a report ending "RESULT: PASS".

2. Open the "Program" folder and double-click FrankiesKaraokeStudio.exe.
   (If Windows says "Windows protected your PC", choose More info, then
   Run anyway: this test copy is not signed.)
   In Settings > Advanced every folder shown must be under
   ...\AppData\Local\Granda\FrankiesKaraokeStudio.

3. Click "Choose Music Folder" and choose the "Test Songs" folder from this
   package. Six test songs appear (one is in a folder with accented letters).

4. Quit (Settings > General > Quit Application). Then double-click
   "Check Test Songs Unchanged.cmd". It must say UNCHANGED.
   If it says CHANGED, stop here and send the report.


PART B - KARAOKE (about 45 minutes)
-----------------------------------
5. "Clicks In Time": the lyrics fill the screen when it starts, and a new
   coloured block appears exactly with each click. Escape shows the controls
   (the song keeps playing); Enter goes back to the lyrics (no restart).
6. "Steady Tone": Key up/down changes the note; Tempo changes the speed.
   Play another song, come back: Key and Tempo are remembered.
7. Settings > Audio: each speaker/headphone is listed once; Test Sound;
   volume. A new output is used from the next song. Try unplugging and
   re-plugging headphones between songs.
8. Search and sort; make two playlists; drag songs in; Autoplay with the
   three "Short Song" tracks plays them one after another.
9. Settings > Shortcuts: shortcuts use Ctrl/Alt/Shift; assign one and try a
   clash (it asks before moving it). Space does nothing by default.
10. Settings > Appearance: sizes 80% to 150%. Also try Windows Settings >
    System > Display > Scale at 100%, 125% and 150%: nothing is cut off, and
    at a size too big for the screen the program uses the largest that fits.
11. The start-up picture shows for about 3.5 seconds; a key or click skips it.
12. Quit and start again 5 times, including while a song is playing.
    Double-click the program while it is open: it says it is already open.
13. Quit, then run "Check Test Songs Unchanged.cmd" again: UNCHANGED.


PART C - ONLY THEN THE REAL KARAOKE DRIVE
-----------------------------------------
14. Connect the drive, open Settings > Library, choose its karaoke folder
    and let the first scan finish (it can take a while for 50,000 songs).
    The program only reads the drive.


IF ANYTHING GOES WRONG
----------------------
Send these two files:
    %LOCALAPPDATA%\Granda\FrankiesKaraokeStudio\frankies-karaoke-studio.log
    installation-check.txt (in this folder)
