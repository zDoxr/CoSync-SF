"""IDA batch step 1: run full auto-analysis on Starfield.exe and save the database.
Usage: idat -A -L<log> -S"analyze_and_save.py" -o<db.i64> Starfield.exe"""
import time
import ida_auto, ida_loader, ida_pro
t = time.time()
ida_auto.auto_wait()
ida_loader.save_database(None, 0)
print("COSYNC: analysis finished in %.0f s" % (time.time() - t))
ida_pro.qexit(0)
