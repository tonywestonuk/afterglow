#!/system/bin/sh
# Allow (2) root for uid 2000 (adb shell) forever (until=0); see Magisk native/src/core/sqlite.cpp
DIR="${0%/*}"
"$DIR/magisk" --sqlite "REPLACE INTO policies (uid,policy,until,logging,notification) VALUES(2000,2,0,1,0)" > "$DIR/afterglow_grant_shell.log" 2>&1
echo "exit=$?" >> "$DIR/afterglow_grant_shell.log"
