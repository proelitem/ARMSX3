# Multijoueur à deux casques, et Time Crisis 4 à deux

Objectif : deux Quest 3, un jeu, chaque joueur voit le jeu dans son casque et vise avec sa
propre manette. Cas de référence : Time Crisis 4 (PS3, GunCon 3, deux joueurs sur le même
écran).

## 1. Architecture retenue : un hôte émule, l'autre casque reçoit

RPCS3 n'a pas de netplay déterministe et ne pourra pas en avoir à court terme : faire tourner
deux instances synchronisées est exclu. La seule architecture viable est celle qu'ARMSX3
possède déjà, le **remote play** : l'hôte émule, encode la vidéo (WebRTC, H.264 matériel),
l'audio (Opus) et reçoit le pad du joueur 2 par un canal de données non fiable, sur le même
Wi-Fi (découverte DNS-SD) ou par Internet (UPnP). Le viewer est une activité 2D ; dans un
casque, Horizon OS l'affiche comme un panneau flottant.

Ce que le dépôt n'avait pas pour un jeu de tir, et qui est livré ici :

| Besoin | Avant | Maintenant |
|--------|-------|------------|
| Un pistolet PS3 | aucun : `Lightgun.kt` appelait des stubs PCSX2 (`Unsupported.note`) | le pad devient un **GunCon 3** (identité USB Namco 0x0B9A/0x0800), celui que Time Crisis 4 attend |
| Une source de visée | aucun gestionnaire de souris Android ; le GunCon 3 du core lit la souris | `android_mouse_handler.h` : une souris par port de pad, pilotée par l'app, réglage « Mouse » forcé à `raw` pour que chaque pistolet lise la sienne |
| Viser dans le casque | rien | le rayon de visée (`aim pose`) de la manette droite coupe l'écran virtuel ; le point d'impact devient la souris 0, la gâchette devient Croix (la gâchette du GunCon 3) |
| Viser depuis le casque du joueur 2 | rien | le pointeur de sa manette sur le panneau du viewer (survol et appui) est envoyé à l'hôte en paquet « aim » ; l'hôte le met dans la souris 1 et la gâchette dans Croix du pad 2 |
| Viser sur téléphone | stubs | la couche tactile existante, rebranchée sur la souris du core |

## 2. Comment jouer à Time Crisis 4 à deux

Hôte (casque 1) : Paramètres > Rendu > « Écran virtuel » et « Pistolet avec la manette
droite » activés, puis lancer le jeu. Ouvrir une session remote play (même Wi-Fi). Le pad 1
est un GunCon 3 ; dès qu'un ami rejoint, le pad 2 en devient un aussi.

Invité (casque 2) : rejoindre la session dans ARMSX3 (viewer). Pointer la manette vers le
panneau vidéo : le survol vise, la gâchette tire.

Dans Time Crisis 4, le pédalier de couverture est sur les boutons A/B du GunCon 3, c'est-à-dire
L1/R1 du pad : les grips des manettes Touch chez l'hôte, et les boutons du pad virtuel chez
l'invité.

## 2 bis. Quelle manette est le pistolet, et les accessoires

- **Main** : le pistolet est la manette Touch dont la gâchette a été pressée en dernier,
  la droite au départ. Un gaucher ou un stock qui tient la manette gauche n'ont rien à régler.
- **Calibration** : bouton menu maintenu + gâchette en visant le centre de l'écran. La
  correction est gardée par main et enregistrée dans `xr_lightgun_offsets.txt` à côté de la
  configuration. Nécessaire pour un stock dont le canon n'est pas l'axe de visée de la
  manette, utile pour quiconque tient l'arme de travers. Refaire la calibration remplace
  l'ancienne.
- **Main fixe** : réglage « Main du pistolet » (Auto / Gauche / Droite) sous l'interrupteur
  pistolet. Auto suit la gâchette ; avec un WonderFitter connecté, Auto veut dire droite, comme
  il est monté dans Tremu.
- **WonderFitter (M17, M416)** : ce n'est pas un périphérique HID mais un accessoire Bluetooth
  LE qui tient une manette Touch. La manette vise ; le pistolet n'envoie que ce que sa
  mécanique fait, en notifications sur la caractéristique FFF1 du service FFF0 (le protocole
  que Tremu parle déjà, `WonderFitterGun.java`) : tir `5B SS 07 07 CK 5D` (trois fois par
  coup, dédoublonné par le numéro de séquence), chargeur `5C MM CK 5D` (00 retiré, 01 inséré),
  culasse `60 MM CK 5D`. Pas de trame « relâché » : la gâchette compte comme tenue 150 ms après
  la dernière trame de tir, ce qui couvre le tir automatique (une trame toutes les 100 ms).
  Côté ARMSX3 (`com.armsx2.xr.WonderFitterGun`) : le tir passe par la gâchette externe du
  core XR (`_armsx3_xrGunTrigger`), donc il tire avec la main du pistolet et sert au geste de
  calibration (menu + tir au centre) ; le chargeur retiré maintient A1 (L1, la pédale relâchée
  de Time Crisis 4 : on se cache et on recharge) ; la culasse tape A1 150 ms. Le pistolet
  n'accepte qu'un client : Tremu le lâche avant de lancer ARMSX3, l'activité immersive le prend
  dans `onResume` et le rend dans `onStop`, et Tremu le reprend. Permissions `BLUETOOTH_SCAN`
  (neverForLocation) et `BLUETOOTH_CONNECT`, demandées par l'activité immersive quand le mode
  pistolet est actif.
- **Autre pistolet Bluetooth/USB (Arkade, Gun4IR, Samco, OpenFIRE...)** : vu par le casque
  comme manette ou souris. La manette Touch montée dessus vise ; sa gâchette est prise sur les
  dispositions courantes (bouton 1 de souris, R2, A, axe gâchette droite) et passe par la même
  gâchette externe ; ses autres boutons arrivent au pad par les gestionnaires de Main (L1/R1 =
  A1/B1 du GunCon 3, Start, Select). Chaque périphérique inconnu est décrit une fois dans le
  journal (nom, sources, identifiants) pour ajuster si une disposition diffère.

## 3. Ce que la latence impose

Le joueur 2 vise sur une image qui a traversé l'encodeur, le Wi-Fi et le décodeur : comptez
40 à 80 ms sur un bon Wi-Fi 5 GHz, davantage sur un réseau chargé. Pour un rail shooter c'est
jouable mais ce n'est pas la précision de l'hôte. Trois leviers, par ordre d'efficacité :

1. **Le Wi-Fi** : routeur à moins de dix mètres, bande 5 GHz dédiée, pas d'autre trafic vidéo.
   La stat `HostStats` du remote play (RTT) dit où l'on en est.
2. **La résolution du flux** : 720p suffit, c'est la sortie native de la PS3.
3. **Un viewer immersif** (phase 2) : afficher le flux directement en couche OpenXR dans le
   casque 2 au lieu du panneau Horizon économise la composition du panneau et donne un
   écran de la même taille que chez l'hôte. Ce n'est pas un gain de latence réseau.

## 4. Phase 2 : le viewer immersif

Aujourd'hui le viewer est un panneau 2D. Le passage en OpenXR demande un rendu propre au
viewer (pas de core) : flux MediaCodec vers une SurfaceTexture, dessinée sur la XrSwapchain
par GLES (XR_KHR_opengl_es_enable) sur un quad identique à celui de l'hôte, et la visée par
le même rayon que l'hôte. Deux à trois jours de travail, à lancer une fois que la phase 1 est
vérifiée sur deux casques : le pipeline hôte, le GunCon 3 et l'injection de la visée ne
changent pas d'une phase à l'autre.

## 5. Ce qui est à vérifier sur le matériel

- Time Crisis 4 démarre-t-il avec un GunCon 3 sur le pad 1 et le reconnaît-il (journal :
  « Adding emulated GunCon3 (controller 0) ») ? Idem pour le pad 2 quand l'ami rejoint.
- La visée : le point d'impact du rayon correspond-il au curseur du jeu ? Le calcul suppose
  que l'écran virtuel est à `distance_m` devant l'origine de l'espace LOCAL, ce qui est la
  géométrie du quad ; un recentrage du casque en cours de jeu déplace les deux ensemble.
- Chez l'invité : Horizon OS livre-t-il bien les survols du pointeur au `SurfaceViewRenderer`
  (`ACTION_HOVER_MOVE`) ? Sinon, seule la visée gâchette enfoncée fonctionne, et il faudra
  lire le pointeur autrement (phase 2 le règle de toute façon).
- Le conseil communautaire d'une limite de 30 fps pour la précision de tir de Time Crisis 4
  sous RPCS3 est à tester sur le casque.

Sources : [support deux GunCon 3 sur PS3](https://consumerist.com/2007/12/05/namco-bandai-wont-sell-accessory-separately), [forum arcade, limite 30 fps](https://forum.arcadecontrols.com/index.php/topic,163489.180/wap2.html).

## 6. Pourquoi le lockstep de Tremu décroche sur Sega et PS2 (audit du 4 novembre 2026)

Lu dans `src/netplay.c`, `src/core.c` et `src/main.c` de Tremu. Le modèle : lockstep
déterministe en étoile autour de l'hôte. Chaque casque fait tourner le même cœur et la même
ROM ; seuls les pistolets voyagent (11 octets par joueur). Un nouveau venu reçoit l'état de
sauvegarde libretro de l'hôte par UDP (morceaux de 1 Ko, 4 par pas de 1 ms), la partie est
figée pour tout le monde pendant le transfert. Toutes les 60 images, chaque casque calcule
un CRC de la RAM système (`retro_get_memory_data`) ; une différence déclenche un nouveau
transfert d'état vers l'invité (au plus trois par minute). Le délai d'entrée de l'invité est
fixé une fois, à l'arrivée, à partir d'un seul aller-retour (2 à 8 images) ; l'hôte attend
l'entrée de chaque invité avant de calculer une image, sans rollback.

Ce modèle est exactement celui qui convient à Terminator 2 sur FBNeo : cœur déterministe,
état de quelques centaines de Ko, image calculée en une fraction de 16 ms, donc du jeu pour
absorber la gigue du Wi-Fi. Tout ce qui suit se dégrade avec la taille de l'état et la charge
CPU, ce qui décrit Dreamcast/Naomi, Saturn et PS2 :

1. **Le CRC de la RAM** (`wg_core_ram_crc`) est calculé bit à bit, sans table, sur toute la
   RAM système, sur le fil de l'émulateur, une fois par seconde. 64 Ko pour la Mega Drive :
   rien. 16 Mo (Dreamcast), 32 Mo (PS2) : 130 à 260 millions d'itérations, soit de l'ordre de
   100 à 300 ms de gel chaque seconde sur le XR2 Gen 2, et pendant ce gel les paquets d'entrée
   ne partent plus, donc l'autre casque cale aussi. C'est la saccade périodique « une fois
   par seconde » que l'on observe. Correction simple : la table de `netplay_file_crc`
   (déjà dans le fichier) et un échantillonnage (un octet sur N pour plafonner à ~1 Mo).
2. **Le transfert d'état** n'est pas compressé et avance de 4 Ko par ms au mieux : un état
   LRPS2 de 50 à 80 Mo prend 15 à 25 s, pour un délai d'abandon de 30 s (`transfer timed
   out`). À la moindre perte (le Wi-Fi), le rattrapage des morceaux manquants dépasse le
   délai : l'invité échoue à rejoindre, ou la resynchronisation échoue. Flycast est dans la
   même zone (quelques dizaines de Mo), Saturn aussi. Corrections : compresser (zlib est dans
   le NDK, les états se compressent 3 à 5 fois), régler le débit et le délai d'abandon sur la
   taille, et ne pas figer les autres joueurs pendant un transfert qui dure plus de quelques
   secondes (faire rejoindre à un point d'arrêt plutôt qu'en cours de partie).
3. **Le déterminisme** : FBNeo, Genesis Plus GX, FCEUmm, Snes9x sont déterministes par
   construction (RetroArch y fait du netplay). LRPS2 (GS et VU1 sur leurs propres fils,
   relectures GS dépendantes du moment) et Flycast (rendu threadé, recompilateur, états de
   sauvegarde fragiles) ne le sont pas ; RetroArch ne propose pas de netplay pour eux. En
   lockstep, toute divergence est une désynchronisation : CRC différent, resynchronisation,
   donc un transfert du point 2 avec gel général, jusqu'à trois fois par minute. Sur PS2
   c'est l'expérience « ça marche un moment puis ça se fige ».
4. **Pas de marge CPU** : avec un délai de 2 images (33 ms), l'hôte attend l'entrée de
   l'invité pour chaque image. Un cœur qui remplit déjà ses 16 ms (PS2, Dreamcast sur
   Quest 3) n'a aucune marge pour la gigue Wi-Fi (pics de 50 à 100 ms courants) : micro-arrêts
   permanents, que le casque en retard impose à l'autre. Le délai n'est jamais réévalué après
   l'arrivée. FBNeo laisse 10 ms de marge par image, ce qui suffit.

**Recommandation** : garder le lockstep pour les cœurs légers et déterministes (Arcade FBNeo,
Mega Drive, NES, SNES, PS1 à vérifier), après les corrections 1 et 2. Pour PS2, Dreamcast,
Naomi, Saturn (et PS3 dans ARMSX3), passer au modèle de ce document : un casque émule, encode
avec MediaCodec et diffuse l'image, l'autre renvoie son pistolet (section 2). Pas de
déterminisme requis, pas de transfert d'état, un seul CPU chargé, et le joueur 2 paie 40 à
80 ms de latence au lieu d'imposer ses gels à l'hôte. Le protocole de visée d'ARMSX3
(`RemotePlayProtocol.kt`, paquet AIM) peut servir tel quel à Tremu, ce qui permettrait à un
invité Tremu de rejoindre un hôte ARMSX3 et inversement.
