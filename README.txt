Hydra Remuxer v1.0.0
====================

Elindeki videolari MKV'ya tasiyan kucuk bir Win32 araci. ffmpeg kullanir,
yeniden encode yapmaz; hizli olmasinin sebebi bu.

KURULUM
  Zip'i bir klasore ac, ffmpeg.exe'yi ayni klasore koy (ya da PATH'te olsun).
  ffmpeg suradan alinir: https://www.gyan.dev/ffmpeg/builds/ (essentials yeter)
  Windows 7 ve ustunde calisir.

KULLANIM
  Videolari pencereye surukle-birak, Baslat'a bas. Hepsi bu.
  Ciktilar giris dosyasinin yanina yazilir: film.mp4 -> film.mkv
  Uzerine yazma yok; isim cakisisa film (1).mkv olur, mkv giriste film.remux.mkv.
  Olusturma + degistirme tarihleri aynen kopyalanir.
  Klasor birakirsan icindeki medya dosyalari taranir.
  Sag ustteki sayi ayni anda calisan is sayisi (varsayilan CPU cekirdegin).
  HDD kullaniyorsan 2'ye indir, SSD'de oldugu gibi birak.

BIRKAC NOT
  Bazi mp4'ler mkv'ya birebir gecmiyor (mov_text altyazi, timecode izleri
  gibi). Program once tam kopya dener, olmazsa sirayla veri izlerini atmayi
  dener, sonra altyaziyi srt'ye cevirir. Ne yaptigini Bilgi sutununa yazar.
  Video/ses her durumda kopyadir, encode yok.

DOGRULAMA
  Her isin sonunda kaynakla cikti karsilastirilir: video kare kare (XXH3),
  ses paket duzeyinde. Kutuyu kaldirirsan bu adim atlanir.
  Ayrica tek basina da calisir:
    HydraRemuxer.exe /check kaynak cikti [/silent]
  Cikis kodu 0=ok, 1=hata. HYDRA_VERIFYLOG'a dosya yolu verirsen
  sonuclari oraya da yazar.

BILINEN SINIRLAR
  - Altyazi icerigi dogrulanmaz. Cok nadir bir karisimda (bitmap + metin
    altyazi bir arada) altyazi dusebilir; o zaman satirda yazar.
  - Derleme: build.bat (MSYS2 UCRT64 + gcc lazim). Butun is remux.c'de
    tek dosyada, yaninda xxhash.h var ( Disaridan alindi, elleme).

SURUM
  v1.0.0 - ilk surum.
  Bir sorun gorursen GitHub Issues'ya yaz.
