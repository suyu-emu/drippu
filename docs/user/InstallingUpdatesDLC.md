# User Handbook - Working with Updates/DLC in Eden

Use this guide when you want to install Updates or DLC for your games in Eden.

<aside>

***NOTE***: This applies to separate Update/DLC files, not "merged" NSP/XCI's which include the base game and Updates/DLC applied on top of them in a single file.  These files work in Eden, but would not require the following steps.

</aside>

**Click [Here](https://evilperson1337.notion.site/Working-with-Updates-DLC-in-Eden-2b057c2edaf681dfb65dfc4dd96980c0) for a version of this guide with images & visual elements.**

---

### Pre-Requisites

1. Eden already setup and configured for your platform.
2. The Update/DLC file(s) you want to install

---

## Installing Updates/DLC

1. Open Eden to the Main Window.
2. Select *File > Install Files to NAND...*.
3. Navigate to the Update/DLC files you want to install.
    <aside>
    
    ***TIP***: You can install multiple files at once by selecting multiple files in this window.
    
    </aside>
    
4. The file(s) will be scanned for validity and then a confirmation window will appear, select *Install* to begin installation.
    <aside>
    
    ***TIP***: You can deselect any that you do not want to install with the checkbox by each entry.
    
    </aside>
    
5. Upon installation, you will get a prompt saying it was installed successfully.    
6. Look at the *Add-Ons* column in the main window, you should now see the additional installed content reflected.

---

## Disabling Updates/DLC

Upon occasion you may find that you want to disable a certain DLC or Update (incompatibility with a mod, causes significant regression, etc.). Luckily the process if very easy to do so.

1. *Right-Click* the game for which you want to disable the additional content.
2. Select *Configure Game.*
3. Uncheck the box next to the DLC or Update you want to disable and hit **OK**.
4. The listing should now reflect that it has been disabled with a **[D]** before the entry.  If you load the game, you will observe that the reported version is not updated (assuming the game reports this information).

---

## Exporting a game with separate updates / DLC

Game Export can bake **owned** separate update and DLC/AOC dumps (or NAND-installed add-ons for that title) into the standalone package. Merged dumps that already contain the update still work; this is for the split-file layout.

This does not help you obtain dumps. Only use files you already own.

1. Open **Export Game**.
2. Choose the base ROM.
3. Under **Updates / DLC**, add the extra NSP/XCI/NCA files, and/or leave **Use NAND-installed add-ons for this title** checked if you already installed them for emulation.
4. The status line lists what will be baked (update version, DLC IDs, and source).
5. Export as usual. The package's `exefs/` (including `romfs.bin`) is the **patched snapshot**. DLC is written under `aoc/<title id>/romfs.bin`.

If an extra file cannot be read, export **stops** and reports the path — it does not package a base-only snapshot while claiming the add-on was baked. The status line only lists an update when **both** ExeFS replace and RomFS BKTR applied. A folder dump of extracted `exefs/` cannot BKTR an update (no Program NCA); export refuses rather than mixing update NSOs with base `romfs.bin`. Use the original NSP/XCI (or a merged dump) as the base ROM.

The standalone runtime does **not** need a post-export NAND install. Updates are already applied to ExeFS/RomFS; DLC is served from the baked `aoc/` tree. `content_baked.txt` in the package repeats the same summary.