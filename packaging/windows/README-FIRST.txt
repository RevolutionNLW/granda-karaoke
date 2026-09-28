FRANKIE'S KARAOKE STUDIO - WINDOWS TEST COPY
=============================================

This is a test copy for checking the program on this laptop. It installs
nothing: everything is in this folder. The program keeps its own settings,
playlists and song list in:

    %LOCALAPPDATA%\Granda\FrankiesKaraokeStudio

(paste that into the File Explorer address bar to open it). It never writes
anything into a music folder.

DO NOT CONNECT THE REAL KARAOKE DRIVE UNTIL STEP 6 HAS PASSED.

1. CHECK THE INSTALLATION
   Double-click "Check Installation.cmd". After about 10 seconds Notepad
   opens with a report that should end with "RESULT: PASS".

2. FIRST START (no music yet)
   Open the "Program" folder and double-click FrankiesKaraokeStudio.exe.
   Open Settings > Advanced and note the folders it shows: all of them
   should be under ...\AppData\Local\Granda\FrankiesKaraokeStudio.
   Quit (Settings > General > Quit Application).

3. USE THE TEST SONGS AS THE MUSIC FOLDER
   Start the program again and choose the "Test Songs" folder in this
   package as the music folder. Six test songs should appear.

4. PLAY, KEY, TEMPO, PLAYLISTS
   "Clicks In Time": a new coloured block must appear on the lyrics
   screen exactly with each click. "Steady Tone": Key up/down must change
   the note. Try Tempo, Escape (controls) and Enter (lyrics), the sound
   output and volume in Settings > Audio, playlists, drag and drop and
   Autoplay with the three "Short Song" tracks.

5. QUIT AND START A FEW TIMES
   Quit and restart the program 5 times, including while a song plays.

6. PROVE NOTHING WAS WRITTEN INTO THE MUSIC FOLDER
   Quit the program, then double-click "Check Test Songs Unchanged.cmd".
   It must say "UNCHANGED". If it says "CHANGED", stop and report it.

Only after step 6 says UNCHANGED, connect the real karaoke drive and
choose its folder in Settings.

If anything goes wrong, send the log file:
    %LOCALAPPDATA%\Granda\FrankiesKaraokeStudio\frankies-karaoke-studio.log
and "installation-check.txt" from this folder.
