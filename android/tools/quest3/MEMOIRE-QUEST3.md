# Mémoire sur Quest 3 : ce qu'un « swap interne » peut et ne peut pas être

Demande de départ : « donner 16 Go à la Quest 3 en utilisant la flash comme swap ».
Ce document dit ce qui est possible sans root, ce qui est déjà fait dans le code, et
ce qui est planifié. Il est honnête sur les limites : le chiffre de 16 Go n'est pas
atteignable, et la plupart de la mémoire d'un jeu PS3 émulé n'est pas « swappable »
par une application.

## 1. Les contraintes, en clair

| Fait | Conséquence |
|------|-------------|
| `swapon` exige CAP_SYS_ADMIN ; une app Android ne l'a pas, la Quest 3 ne se roote pas | Aucun fichier de swap noyau ne peut être créé par ARMSX3. |
| Horizon OS a très probablement une zram (swap compressé en RAM) | Les pages anonymes de l'app y vont déjà, sans rien faire ; c'est le seul swap qui existera. Le script `info` du spike le mesure (`/proc/swaps`, `/sys/block/zram*`). |
| La mémoire GPU de l'Adreno est la RAM système (allouée par KGSL) | Le cache de textures et les render targets (jusqu'à 2 à 3 Go) ne peuvent être ni swappés ni adossés à un fichier. On peut seulement les réduire. |
| Un mapping `MAP_SHARED` sur un fichier est « swappable » par le cache de pages | Mais le noyau réécrit les pages sales en continu (dirty writeback, ~30 s) : pour la RAM invitée PS3, modifiée à chaque image, c'est des dizaines de Go/h d'écriture flash. Inacceptable par défaut. |
| Le code JIT (PPU/SPU compilés) est gros, froid et déjà sur flash (cache) | Candidat sérieux à un mapping fichier **en lecture seule** : pages propres, évincées gratuitement et rechargées à la demande, zéro usure. Demande une intervention dans le gestionnaire mémoire LLVM. |

Conclusion : le levier n'est pas « plus de mémoire » mais « ne jamais se faire tuer » :
savoir où va la mémoire, rendre ce qui peut l'être quand l'OS le demande, et dimensionner
les caches pour un budget de ~5 Go réellement disponibles.

## 2. Ce qui existait déjà dans le core

- Un chemin d'éviction GPU complet, `VKGSRender::on_vram_exhausted(severity)`, appelé par
  l'allocateur Vulkan quand une allocation échoue : purge du cache de textures (moderate),
  retrait des render targets périmés (severe), hard sync et éviction large (fatal).
- Un plafond VMA (`vramLimitMb`, 2048) et un budget interne du cache de textures. Les
  commentaires de `Settings.kt` expliquent pourquoi baisser le plafond fait échouer les
  allocations plus tôt sans rien libérer : c'est un mur, pas un budget.
- Aucune entrée côté système : l'OS pouvait dire « rends de la mémoire » (`onTrimMemory`),
  personne n'écoutait, et rien ne mesurait la répartition de la mémoire du processus.

## 3. Phase 1, livrée : télémétrie et gouverneur

- `MemoryWatch.kt` : toutes les 15 s pendant un jeu, `ActivityManager.MemoryInfo`
  (disponible, total, seuil de kill, drapeau lowMemory) est envoyé au core, qui l'inscrit
  dans ARMSX3.log avec la répartition du processus lue dans `/proc/self/status` (VmRSS,
  RssAnon, RssFile, RssShmem, VmSwap) et l'état du swap système (`/proc/meminfo`).
- Pression : `onTrimMemory` (niveaux RUNNING_MODERATE/LOW/CRITICAL) et le passage sous
  1,5 fois le seuil de kill déclenchent une demande de trim que le thread RSX applique à
  l'image suivante via `on_vram_exhausted` (moderate ou severe, jamais fatal). Limité à
  une demande modérée par 20 s.
- Le script `quest3-driver-spike.sh info` affiche la mémoire totale, le swap, la zram et les
  seuils du lowmemorykiller du casque.

Ligne typique attendue dans ARMSX3.log :

```
Memory: avail 1830MB of 7612MB (kill threshold 1152MB) | VmRSS=4210MB RssAnon=2980MB RssFile=410MB RssShmem=820MB VmSwap=0MB MemAvailable=1830MB SwapTotal=3072MB SwapFree=2900MB
```

Lecture : RssShmem est la mémoire invitée PS3 (memfd), RssAnon le tas, le JIT et les
allocations KGSL comptées côté CPU, VmSwap dit si la zram nous prend quelque chose.

## 4. Phase 2 : profil mémoire Quest, à partir des mesures

Avec dix minutes de jeu sur trois titres (un léger, un lourd, un 3D natif), décider :

- le budget du cache de textures et le plafond VMA pour un casque (probablement 1536 Mo),
  en vérifiant sur les logs qu'aucune allocation n'échoue avant que la pression ne tombe ;
- si la 3D native double la pression (deux render targets) et mérite ses propres défauts ;
- si le seuil de trim proactif (1,5 × threshold) est trop tôt ou trop tard.

Ces valeurs iront dans `HeadsetProfile.freshInstallDefaults`.

## 5. Phase 3 : code JIT adossé au cache sur flash (le vrai « swap » utile)

Le cache PPU/SPU compilé d'un gros jeu dépasse 1 Go et n'est touché qu'à l'exécution de
chaque fonction. Aujourd'hui il est chargé dans de la mémoire anonyme exécutable. Le
mapper directement depuis les fichiers du cache (`MAP_PRIVATE`, PROT_READ|EXEC) rend
ces pages propres : le noyau les évince sous pression et les relit depuis la flash, sans
écriture. C'est le seul mécanisme de type swap qui ne coûte ni usure ni privilèges.

Travail : un `RTDyldMemoryManager` (ou JITLink) d'ARMSX3 qui, pour un objet déjà en
cache, mappe le fichier au lieu de copier ses sections, avec relocations appliquées en
copie privée (donc seules les pages reloquées deviennent sales). À chiffrer après la
phase 2 : si les mesures montrent que RssAnon est dominé par le JIT, cela vaut
l'investissement ; si c'est le cache de textures, non.

## 6. Ce qui n'est pas prévu

- Un swap fichier pour la mémoire invitée ou le tas : usure flash et latence I/O sur le
  chemin chaud, pour un gain nul par rapport à la zram.
- `userfaultfd` : un pager en espace utilisateur ferait la même chose que le cache de
  pages, en plus lent et plus fragile.
