Let's improve some stuff about @007_esp_s3_touch_flashcard_ble.ino, UI/UX wise.

Utilize the @icons/. These are tiny 8x8 monochrome icons, it probably makes more sense to convert them to straight bytestrings or bitmaps or whatever and load them straight into source. Follow recommendations here, just know I'm not married about loading these actual png files.

## Changes

- let's simplify the screen: the first 8 rows are reserved for general icons. The last 8 rows are reserved for icons also. Then, from the remaining screen,  the complete upper half, full width, should be reserved for the front of a flashcard. the foll lower half, full, width, should be reserved for the back. May or may not have impact on the dithering logic in @sync.html.
    - before reveal, show the front of the card, and centered in bottom icon area show the "reveal" icon button. However the whole (empty) back reserved space is sensitive as a button for revealing, also.
    - after reveal, render the back in the reserved space, and also render the wrong and correct icon buttons in the bottom left and bottom right, respectively. However, whole reserved space left part should be trigger space for "wrong", and whole reserved space right part should be trigger space for "right", also
- simplify the bluetooth connection flow: instead of the hardcoded "settings" icon in the top right, show icons/connect.png in the top right. Once clicked, automatically clear the whole screen, and just show a x/y centered string "Sync Mode" and below that a text button "Restart".
- use exclusively partial refresh in normal flashcard operation. When entering sync mode, do a full refresh. Also, add icon button to the top left (reveal) that triggers a hard refresh (so user can use it themselves when artifacts become annoying)

Implement cleanly. Feel free to use this to factor out the giant ino files into more clean components. Keep clear track of your state machine. Web search ESP-related recommendations if relevant. Check README for context.