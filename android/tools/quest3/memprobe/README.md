# memprobe : mesurer la mémoire réelle d'une app sur Quest 3

Petite application jetable qui répond aux questions laissées ouvertes dans
`../MEMOIRE-QUEST3.md`, avec des chiffres relevés sur le casque :

1. **Où Horizon OS tue une application de premier plan**, en Mo de mémoire anonyme (la
   limite documentée est d'environ 5,75 Go), et si la zram en absorbe une partie.
2. **Si la mémoire adossée à un fichier compte dans cette limite** : un `mmap` MAP_SHARED
   de 12 Go sur la flash survit-il là où 6 Go d'anonyme sont tués ?
3. **Ce que coûte un rechargement depuis la flash** : latence des lectures aléatoires après
   un remplissage qui dépasse la RAM.
4. **Ce que la flash subirait** : volume réellement écrit par le noyau pendant 60 s de
   réécriture d'un ensemble de travail de 1 Go (delta de `nr_written`).

## Compilation

Même SDK qu'ARMSX3 (AGP 9.2.1, NDK 29.0.14206865, CMake 3.30.5) :

```bash
cd android/tools/quest3/memprobe
./gradlew.bat :app:assembleRelease      # ./gradlew sous WSL/Linux
adb install -r app/build/outputs/apk/release/app-release.apk
```

## Protocole

Casque posé, aucune autre app ouverte. Lancer memprobe depuis Sources inconnues. Pour chaque
bouton, attendre la fin ou la mort de l'app, puis relancer memprobe : la fin du journal
précédent s'affiche en tête.

1. **ANON jusqu'au kill** : la dernière ligne `filled=` est la limite. Noter `VmSwap` et
   `pswpout` : non nuls = la zram travaille.
2. **MEMFD jusqu'au kill** : idem pour la mémoire partagée (celle de RPCS3).
3. **FILE 12 Go + réécriture** : noter si le remplissage dépasse la limite trouvée en 1
   (alors les pages de fichier ne comptent pas, ou pas pareil), `RssFile` au fil du
   remplissage, les percentiles `random reads`, et le delta `nr_written` autour de la
   réécriture : c'est l'usure.

Récupérer le journal :

```bash
adb pull /sdcard/Android/data/com.armsx3.memprobe/files/memprobe.log
```

## Lecture des résultats

| Observation | Conséquence |
|-------------|-------------|
| ANON tué vers 5,5 à 6 Go, FILE tué au même RSS total | Les pages de fichier comptent : aucun swap interne ne contourne la limite. Fin du sujet. |
| FILE atteint 12 Go, p99 de lecture < 500 µs, nr_written modéré | Un pager par fichier est viable pour de la mémoire froide ; à intégrer dans l'émulateur cible, pas dans ARMSX3. |
| FILE atteint 12 Go mais nr_written > 1 Go/min en réécriture | Viable seulement pour des données rarement modifiées (code traduit, assets), jamais pour la RAM invitée. |
| VmSwap croît sur ANON | Horizon OS a une zram active ; la mémoire anonyme compressible est déjà « swappée ». |
