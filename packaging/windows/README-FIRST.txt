FRANKIE'S KARAOKE STUDIO - WINDOWS TEST CHECKLIST
==================================================
About 1 to 1.5 hours, plus waiting for the real drive's first scan.

  +-----------------------------------------------------------------+
  |  KEEP FRANKIE'S REAL KARAOKE DRIVE UNPLUGGED.                    |
  |  It is connected only at the very end (PART F), and only if     |
  |  everything before it has passed.                               |
  +-----------------------------------------------------------------+

This test copy installs nothing. The program keeps all its own files in
    C:\Users\<name>\AppData\Local\Granda\FrankiesKaraokeStudio
and never writes anything into a music folder.

Work down the list and tick each box. Where it says STOP, stop.


PART A - INSTALL AND SAFETY (about 20 minutes)
----------------------------------------------
[ ] A1  Copy the ZIP onto the laptop (USB stick or download).
        Right-click the ZIP > Properties. At the bottom of the General tab,
        tick "Unblock" if it is there > OK.
        Right-click the ZIP > Extract All... > change the folder to  C:\
        > Extract. You get the "test folder":
            C:\FrankiesKaraokeStudio-Windows-x64
        It holds: README-FIRST.txt, Check Installation.cmd,
        Check Test Songs Unchanged.cmd, Program, Test Songs, checks.

[ ] A2  The expected Windows warning. This test copy is not signed, so
        Windows may show a blue box "Windows protected your PC" the first
        time you open something from the test folder. That is expected
        for THIS test copy, which we built ourselves:
          click "More info", check the name is FrankiesKaraokeStudio.exe
          or one of the two .cmd files from the test folder, then
          click "Run anyway".
        Only do this for files in the test folder. If Windows or an
        antivirus program says a file is harmful or removes it: STOP and
        report it. Never switch off the antivirus.

[ ] A3  Double-click "Check Installation.cmd". A black window opens.
        After 10 to 20 seconds you hear a short beep and Notepad opens.

[ ] A4  EXPECT: the last line in Notepad is   RESULT: PASS
        and you heard the beep (turn the laptop volume up if not, and run
        it again). Every other line says PASS or INFO.
        Close Notepad and the black window.

[ ] A5  Open the "Program" folder and double-click FrankiesKaraokeStudio.exe.
        A start-up picture shows for about 3.5 seconds, then the program
        fills the screen and offers "Choose Music Folder".

[ ] A6  Open Settings (the gear button, top right) and choose Advanced.
        EXPECT: every folder listed starts with
            C:\Users\<name>\AppData\Local\Granda\FrankiesKaraokeStudio
        None may be in Documents, on the Desktop, in Program Files, or in
        the test folder. Close Settings.

[ ] A7  Click "Choose Music Folder" and choose the "Test Songs" folder inside
        the test folder.

[ ] A8  EXPECT: 6 songs appear within a few seconds (artists: Another
        Tester, Björk Tëster, Test Singer, Zebra Band). Wait until the
        status line under the list simply says "6 songs".

[ ] A9  Quit: Settings > General > Quit Application.

[ ] A10 Double-click "Check Test Songs Unchanged.cmd".

[ ] A11 REQUIRE: it says   UNCHANGED

   IF ANY STEP IN PART A FAILS:
       STOP.  DO NOT CONNECT THE REAL KARAOKE DRIVE.
       Send the files listed at the end of this checklist.


PART B - KARAOKE (about 35 minutes)
-----------------------------------
Start the program again (Program\FrankiesKaraokeStudio.exe).

[ ] B1  Start-up picture: about 3.5 seconds, then the program. Quit and
        start again, and press a key while the picture shows: it goes at once.

[ ] B2  Search: type  zeb  -> only Zebra Band. Clear the box.
        Type  bjork  (no accents) -> finds "Straße Song" by Björk Tëster.

[ ] B3  Sort (the sort box above the list):
          Artist A → Z : Another Tester first ... Zebra Band last
          Artist Z → A : Zebra Band first ... Another Tester last
          Song A → Z   : Clicks In Time first ... Straße Song last
          Song Z → A   : the other way round

[ ] B4  THE LYRICS TIMING CHECK. Double-click "Clicks In Time"
        (or select it and click "Sing This Song").
        EXPECT: the lyrics screen fills the display straight away.
        This test song clicks once a second, and with every click a new
        coloured square appears on the lyrics screen.
          LOOK AND LISTEN for 20 seconds:
          - each square appears together with its click;
          - a square must NEVER appear before its click (lyrics must never
            run ahead of the music). That would be a FAIL;
          - a square that is clearly late (more than a blink) is also worth
            writing down.
        The screen clears every 16 squares; that is normal.

[ ] B5  While it plays:
          Escape -> the controls show and the song KEEPS playing.
          Pause  -> sound and squares stop. Play -> both continue from
                    the same place.
          Enter  -> back to the lyrics, the song does NOT start again.
          Stop   -> back to the beginning.

[ ] B6  Key: play "Steady Tone" (one long note). Key + three times: the note
        goes up each time. Key - back down. Reset.
        Tempo: play "Clicks In Time". Tempo - a few times: the clicks come
        more slowly AND the squares still come with the clicks. Reset.
        Play another song, then "Steady Tone" again: the Key you left it
        on is remembered.

[ ] B7  Playlists: click + and name it "Test A". Drag the three
        "Short Song" tracks from the library into it. Move one with
        Up/Down and by dragging. Remove one; put it back with
        "Add to Playlist". Make a second playlist "Test B" and switch
        between the two tabs.

[ ] B8  Autoplay: in "Test A" set "Autoplay: On" and play its first song
        (each is 12 seconds). The others play by themselves in list order,
        then it stops. A song started from the library does NOT go on to
        another song.

[ ] B9  Sort box -> Most Played: the songs played most are at the top.
        Recently Played: the last one played is at the top.

[ ] B10 Settings > Shortcuts:
          give "Play / Pause" the key F5 and try it during a song;
          then give F5 to "Stop" as well: it asks before moving it;
          give "Exit" the keys Ctrl+Q (used in Part D).
        Space does nothing (by design).

[ ] B11 Settings > Appearance: the size, with - and +, from 80% to 150%.
        Everything grows and shrinks at once; nothing is cut off.
        "This is the largest size that fits this screen" may show on a
        small screen: that is expected. Reset to 100%.

[ ] B12 Settings > Audio: "Play a Test Sound" -> a beep. During a song move
        "Music volume": quieter and louder. The output list shows
        "The computer's usual output" and each speaker/headphone ONCE.


PART C - THINGS ONLY THIS LAPTOP CAN SHOW (about 15 minutes)
-------------------------------------------------------------
[ ] C1  Built-in speakers: clear sound, no crackle, at low and high volume.
[ ] C2  Headphones (only if there is a socket and a pair to hand): choose
        them in Settings > Audio; the NEXT song plays on them. Unplug them
        between songs: the next song goes back to the usual output (a
        message may say they are not connected). No crash.
[ ] C3  Bluetooth: only if convenient. Not needed to pass.
[ ] C4  Windows display scaling: Windows Settings > System > Display >
        Scale. Try each size Windows offers (for example 100%, 125%,
        150%) with the program open: it adjusts, nothing is off the edge,
        text is sharp. Put Windows back to its recommended size.
[ ] C5  Full screen: the lyrics cover the whole screen, no taskbar on top.
        Alt+Tab away and back works.
[ ] C6  Lyrics timing by eye: play "Clicks In Time" to its end (1 minute)
        and watch as in B4.


PART D - QUITTING, AGAIN AND AGAIN (about 10 minutes)
------------------------------------------------------
Each time the program must close within a few seconds, with no error box
and no "Not responding". (In full screen there is no close button:
use Alt+F4 or Settings > General > Quit Application.)
[ ] D1  Start, wait 10 seconds, Alt+F4. Do this 5 times.
[ ] D2  Start and quit within 2 seconds (during the start-up picture).
[ ] D3  Settings > Library > "Rescan", close Settings, then quit
        straight away (Alt+F4).
[ ] D4  Quit while a song is playing.
[ ] D5  Settings > General > Quit Application.
[ ] D6  Ctrl+Q (the Exit shortcut from B10).
[ ] D7  With the program open, double-click FrankiesKaraokeStudio.exe
        again: a message says it is already open. OK. The open one carries
        on normally.
[ ] D8  Quit, then run "Check Test Songs Unchanged.cmd": UNCHANGED.


PART E - THE GATE
-----------------
ONLY CONNECT FRANKIE'S REAL KARAOKE DRIVE IF EVERY BOX IS TICKED:
[ ] Check Installation said  RESULT: PASS  (A4)
[ ] Check Test Songs Unchanged said  UNCHANGED  (A11 and D8)
[ ] Settings > Advanced showed only ...\AppData\Local\... folders (A6)
[ ] Songs played with sound and the squares came with the clicks (B4-B6)
[ ] Every quit in Part D was clean
[ ] No message ever said a folder was refused, or mentioned a music
    folder and the program's own folder together


PART F - FIRST TIME WITH THE REAL DRIVE (15 minutes plus the scan)
-------------------------------------------------------------------
[ ] F1  Close other music programs. When the drive is plugged in and
        Windows asks what to do with it, choose "Take no action" (or close
        the box). Do not double-click songs on the drive in File Explorer:
        other programs can leave hidden files on a drive (this program
        never does).
[ ] F2  Plug in the drive. In the program: Settings > Library > Change...
        and choose the karaoke folder on the drive.
[ ] F3  Do NOT rename, move or delete anything on the drive.
        Let the first scan finish; for about 50,000 songs this can take
        30 minutes or more. You can search while it works.
[ ] F4  Play 3 to 5 songs Frankie knows well: sound, lyrics, Key, Tempo.
[ ] F5  Unplug and reconnect, only when no song is playing and the scan
        has finished. If "Work out song keys in the background" is turned
        on (Settings > Metadata), turn it OFF first: Windows may refuse to
        eject the drive while a song is being read for its key.
        Then in File Explorer right-click the drive > Eject, then
        unplug. The status line says "Music drive not connected".
        Plug it back in: within a few seconds the library is connected
        again and songs play. (If Windows gives the drive a different
        letter, choose the folder again in Settings > Library > Change...;
        playlists and Key/Tempo are kept.)
[ ] F6  Quit (Settings > General > Quit Application).


ABOUT SONG NAMES READ FROM TITLE SCREENS (not a Windows fault)
--------------------------------------------------------------
On the Mac the program can read song names from a song's title screen
(Apple's own text recognition). Windows does not have it, so that option
is not shown on Windows. That is expected. Names already read on the Mac
can be brought over: on the Mac, Settings > Metadata > "Save to a File...";
on Windows, Settings > Metadata > "Read from a File...".


IF ANYTHING GOES WRONG
----------------------
Write down what you did and what you saw, and send:
    %LOCALAPPDATA%\Granda\FrankiesKaraokeStudio\frankies-karaoke-studio.log
    %LOCALAPPDATA%\Granda\FrankiesKaraokeStudio\frankies-karaoke-studio.log.1
    installation-check.txt (in the test folder)
(Paste %LOCALAPPDATA%\Granda\FrankiesKaraokeStudio into the File Explorer
address bar to open that folder.)
