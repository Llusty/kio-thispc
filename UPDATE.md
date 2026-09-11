# Aktualizacja do 0.18.0

```bash
cd ~/Pobrane
rm -rf kio-thispc
unzip kio-thispc-0.18.0.zip
cd kio-thispc
chmod +x install.sh
./install.sh
```

## 0.18.0 — konflikty operacji plikowych

Kopiowanie, przenoszenie, Drag & Drop, „Wyślij do” i zmiana nazwy korzystają teraz z interaktywnej obsługi konfliktów KIO/KIOWidgets. Konflikt nie kończy już operacji natychmiastowym błędem: użytkownik może zastąpić, pominąć, zmienić nazwę/sugerować nową nazwę oraz użyć wariantów dla wszystkich elementów, gdy KIO je udostępnia. Anulowanie dialogu konfliktu jest traktowane jako normalne anulowanie operacji.
