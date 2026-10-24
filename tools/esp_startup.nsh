@echo -off
if exist FS1:\efi\boot\bootia32.efi then
FS1:\efi\boot\bootia32.efi
else
if exist FS0:\efi\boot\bootia32.efi then
FS0:\efi\boot\bootia32.efi
endif
endif
