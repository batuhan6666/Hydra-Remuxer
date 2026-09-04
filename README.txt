RemuxMKV - Win32 API + ffmpeg remux araci (hedef: hep MKV)
=======================================================

KULLANIM
  1. RemuxMKV.exe'yi calistirin (ffmpeg.exe ayni klasorde veya PATH'te olmali).
  2. Videolari pencereye surukleyip birakin (dosya veya klasor).
     - Dosya birakma: her sey kabul edilir, ffmpeg ne acarsa.
     - Klasor birakma: bilinen medya uzantilari taranir.
     - "Dosya Ekle" dugmesi de ayni isi yapar.
  3. Paralellik sayisini secin (varsayilan = CPU cekirdek sayisi).
  4. "Baslat"a basin. Ciktilar giris dosyasinin yanina .mkv olarak yazilir.
     - ornek: film.mp4  -> film.mkv
     - giris zaten .mkv ise: film.remux.mkv (uzerine yazmaz)
     - isim cakisiyorsa: film (1).mkv, film (2).mkv ...
  5. Olusturma + degistirme tarihleri giris dosyasindan kopyalanir.

UYUMLULUK MODU (otomatik)
  Bazi MP4 dosyalari MKV'ye birebir kopyalanamaz (ornegin mov_text altyazi
  veya timecode/veri izleri); ffmpeg o zaman "Could not write header"
  hatasi verir. Program once tam kopya dener, olmazsa sirayla sunlari
  dener (video/ses her adimda kopya kalir, re-encode yok):
    1. veri izlerini at
    2. altyaziyi srt'ye cevir
    3. son care: sadece video+ses+ekleri tasi
  Hangi adim ise yaradiysa Bilgi sutununda yazar.

XXH3 DOGRULAMA (varsayilan acik, "XXH3 ile dogrula" kutusuyla kapatilabilir)
  Her remux sonrasi kaynak ile cikti karsilastirilir:
  - Video: iki dosya da ham karelere cozulur (-vsync 0), her kare XXH3_64
    ile hash'lenip birebir karsilastirilir. Kare sayisi/sirasi/icerigi
    tutmazsa DOGRULAMA HATASI verilir (hatali kare numarasiyla).
  - Ses: paket yukleri akan XXH3 + toplam uzunluk ile karsilastirilir.
  - Altyazi/ekler kapsam disidir (codec donusebilir, bayt karsilastirma
    anlamsiz olurdu).
  Maliyet: dosyanin bir kez decode edilmesi kadar sure (bu makinede 1080p
  H264 icin ~111 kare/sn olculdu); kuyruktaki diger dosyalarin remux'uyla
  ortusur. Hata durumunda cikti SILINMEZ, satirda hata yazilir.
  Bagimsiz kullanim: RemuxMKV.exe /check kaynak cikti [/silent]
  Cikis kodu: 0=ok, 1=hata. REMUXMKV_VERIFYLOG ortam degiskenine dosya yolu
  verilirse sonuclar oraya da yazilir.

HIZ NOTLARI
  - Re-encode YOK: ffmpeg her zaman "-c copy" ile calisir.
  - Dusuk probe degerleri (-probesize 5M -analyzeduration 5M) acilisi hizlandirir.
  - ffmpeg process onceligi ABOVE_NORMAL, GUI NORMAL kalir.
  - HDD kullaniyorsaniz paralelligi 2'ye dusurun; SSD'de CPU sayisinda birakin.

TOPLU / KOMUT SATIRI
  RemuxMKV.exe video1.mp4 video2.avi "D:\arsiv"
  Arguman verilen dosyalar otomatik kuyruga girer ve islem kendiliginden baslar.

GEREKSINIM
  - Windows 7+ (long path destegi icin Win10 1607+ onerilir)
  - ffmpeg.exe (https://www.gyan.dev/ffmpeg/builds/ full veya essentials)

DERLEME
  build.bat'i calistirin veya:
    windres resource.rc -o resource.o
    gcc -O3 -march=native -flto -s -municode -mwindows -Wall -o RemuxMKV.exe remux.c resource.o -lcomctl32 -lcomdlg32 -lshell32 -lshlwapi
