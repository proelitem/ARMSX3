#!/usr/bin/env bash
# ARMSX3 -- Quest 3 driver spike (point 1 du plan Quest 3 / Vulkan / stéréo).
#
# Compare, sur un casque Meta Quest 3 / 3S relié en adb, le driver Vulkan d'origine
# (Qualcomm, /vendor/lib64/hw/vulkan.adreno.so) et plusieurs packs Turnip chargés par
# libadrenotools, en collectant pour chaque essai les logs qui répondent aux trois
# questions du spike :
#   A. le driver demandé est-il celui qui tourne ?       -> "Vulkan driver identity"
#   B. le jeu rend-il sans perdre le device ?            -> VK_ERROR_*, DEVICE_LOST, crash
#   C. ce driver peut-il nourrir un compositeur OpenXR ? -> "XR readiness" (patch device.cpp)
#
# Fonctionne depuis Git Bash / WSL / macOS / Linux avec `adb` dans le PATH.
#
# Usage :
#   quest3-driver-spike.sh info                   identité du casque, OS, driver stock, GPU
#   quest3-driver-spike.sh install <apk>          installe l'APK (variante a13-sdk33 conseillée)
#   quest3-driver-spike.sh push-drivers           télécharge les packs Turnip et les pousse dans
#                                                 /sdcard/Download/armsx3-drivers/ (import in-app)
#   quest3-driver-spike.sh run <label>            vide logcat, lance l'app, attend votre test,
#                                                 puis rapatrie logcat + ARMSX3.log + session.log
#                                                 dans results/<label>/ et écrit un résumé
#   quest3-driver-spike.sh summary                un verdict par essai, en tableau
#
# Variables d'environnement optionnelles :
#   ADB                 binaire adb (défaut: adb)
#   ANDROID_SERIAL      numéro de série si plusieurs appareils
#   ARMSX3_PKG          package (défaut: com.armsx3 ; com.armsx3.play pour la variante Play)
#   ARMSX3_DATA_ROOT    racine de données choisie dans l'app, si différente du défaut
#   RESULTS_DIR         dossier des résultats (défaut: ./results)

set -euo pipefail

ADB="${ADB:-adb}"
PKG="${ARMSX3_PKG:-com.armsx3}"
ACTIVITY="com.armsx2.BootSplashActivity"
RESULTS_DIR="${RESULTS_DIR:-results}"
DRIVERS_DIR="${DRIVERS_DIR:-drivers}"
DEVICE_DRIVER_DIR="/sdcard/Download/armsx3-drivers"

# Packs à comparer. Tous au format adrenotools (meta.json + libvulkan_freedreno.so à la racine),
# donc importables par Paramètres > Backend > Driver GPU > Importer dans ARMSX3.
#   - K11MCH1 R7     : référence communautaire de stabilité sur Adreno 7xx (Mesa 26.0, déc.)
#   - WN-Turnip 1.19 : Mesa main (26.3-devel, 30 sept.), variante balanced (pas de PWR_MAX)
#   - Banners A740   : build automatisé Mesa main du 30 oct., variante standard a6xx/a7xx
# StevenMXZ v33 n'est volontairement PAS dans la liste : c'est un build Gen8 (Adreno 8xx).
DRIVER_URLS=(
  "https://github.com/K11MCH1/AdrenoToolsDrivers/releases/download/v26.0.0-rc07/Turnip_v26.0.0_R7.zip"
  "https://github.com/WinNative-Emu/Drivers/releases/download/v1.19/WN-Turnip-1.19-b_Axxx.zip"
  "https://github.com/niilo/Banners-Turnip-A740/releases/download/v27.0.0-20261030/Turnip-v27.0.0-20261030.zip"
)

die() { echo "ERREUR: $*" >&2; exit 1; }
log() { echo "[spike] $*"; }

need_device() {
  "$ADB" get-state >/dev/null 2>&1 || die "aucun appareil adb (mode développeur + débogage USB activés dans le casque ?)"
}

prop() { "$ADB" shell getprop "$1" 2>/dev/null | tr -d '\r'; }

cmd_info() {
  need_device
  local model; model="$(prop ro.product.model)"
  echo "== Appareil =="
  echo "model            : $model"
  echo "manufacturer     : $(prop ro.product.manufacturer)"
  echo "device           : $(prop ro.product.device)"
  echo "android          : $(prop ro.build.version.release) (API $(prop ro.build.version.sdk))"
  echo "build            : $(prop ro.build.display.id)"
  echo "horizon os       : $(prop ro.vendor.build.version.incremental) / $(prop ro.build.version.incremental)"
  echo "soc              : $(prop ro.soc.model) $(prop ro.hardware)"
  echo "kernel           : $("$ADB" shell uname -r 2>/dev/null | tr -d '\r')"
  echo "abi              : $(prop ro.product.cpu.abilist)"
  case "$model" in
    *Quest*|*quest*) ;;
    *) echo "ATTENTION: ce n'est pas un Quest ($model)";;
  esac
  echo
  echo "== GPU (KGSL) =="
  "$ADB" shell 'for f in gpu_model max_gpuclk gpuclk min_clock_mhz max_clock_mhz; do printf "%-14s " $f; cat /sys/class/kgsl/kgsl-3d0/$f 2>/dev/null || echo "?"; done' | tr -d '\r'
  echo
  echo "== Driver Vulkan d'origine =="
  "$ADB" shell 'ls -la /vendor/lib64/hw/ 2>/dev/null | grep -i vulkan' | tr -d '\r' || echo "(non listable)"
  echo "props vulkan     : $("$ADB" shell 'getprop | grep -iE "vulkan|hwui.renderer|gpu"' | tr -d '\r' | tr '\n' ' ')"
  echo
  echo "== vkjson (si dispo) =="
  "$ADB" shell cmd gpu vkjson 2>/dev/null | tr -d '\r' | grep -oE '"(driverVersion|apiVersion|deviceName|driverName|driverInfo)"[^,]*' | head -8 || echo "(cmd gpu vkjson indisponible)"
  echo
  echo "== ARMSX3 =="
  if "$ADB" shell pm path "$PKG" >/dev/null 2>&1; then
    echo "installé         : $("$ADB" shell dumpsys package "$PKG" | tr -d '\r' | grep -m1 versionName | sed 's/^ *//')"
  else
    echo "installé         : non ($PKG)"
  fi
}

cmd_install() {
  need_device
  local apk="${1:-}"; [[ -f "$apk" ]] || die "APK introuvable: $apk"
  log "installation de $apk"
  "$ADB" install -r -g "$apk"
  log "OK. Dans le casque: Bibliothèque > Sources inconnues > ARMSX3. Accordez l'accès au stockage au premier lancement."
}

cmd_push_drivers() {
  need_device
  mkdir -p "$DRIVERS_DIR"
  for url in "${DRIVER_URLS[@]}"; do
    local name="${url##*/}"
    if [[ ! -s "$DRIVERS_DIR/$name" ]]; then
      log "téléchargement $name"
      curl -fL --retry 3 -o "$DRIVERS_DIR/$name" "$url" || die "échec téléchargement $url"
    fi
    if ! unzip -l "$DRIVERS_DIR/$name" 2>/dev/null | grep -qE 'libvulkan.*\.so'; then
      log "ATTENTION: $name ne contient pas de libvulkan*.so à première vue"
    fi
  done
  "$ADB" shell mkdir -p "$DEVICE_DRIVER_DIR"
  for f in "$DRIVERS_DIR"/*.zip; do
    log "push $(basename "$f")"
    "$ADB" push "$f" "$DEVICE_DRIVER_DIR/" >/dev/null
  done
  log "packs disponibles dans le casque sous $DEVICE_DRIVER_DIR"
  log "Dans ARMSX3: Paramètres > Backend > Driver GPU > Importer un driver, choisir le zip, puis Appliquer et redémarrer."
}

# Racine de données ARMSX3 dans le casque: <externalFilesDir> par défaut, sinon ce que l'utilisateur
# a choisi dans l'assistant. ARMSX3.log vit dans <racine>/cache/, session.log dans <externalFilesDir>/logs/.
find_core_log() {
  if [[ -n "${ARMSX3_DATA_ROOT:-}" ]]; then
    echo "${ARMSX3_DATA_ROOT%/}/cache/ARMSX3.log"; return
  fi
  local hit
  hit="$("$ADB" shell "find /sdcard/Android/data/$PKG /sdcard/ARMSX3 /sdcard/Download -maxdepth 4 -name ARMSX3.log 2>/dev/null | head -1" | tr -d '\r')"
  [[ -n "$hit" ]] && echo "$hit" || echo "/sdcard/Android/data/$PKG/files/cache/ARMSX3.log"
}

cmd_run() {
  need_device
  local label="${1:-}"; [[ -n "$label" ]] || die "usage: run <label>  (ex: stock, k11-r7, wn-1.19, banners-1030)"
  local out="$RESULTS_DIR/$label"
  mkdir -p "$out"

  log "essai '$label' -> $out"
  "$ADB" logcat -c
  "$ADB" shell am force-stop "$PKG" >/dev/null 2>&1 || true
  sleep 1
  "$ADB" shell am start -n "$PKG/$ACTIVITY" >/dev/null

  cat <<MSG

  Dans le casque, maintenant :
    1. Paramètres > Backend : Vulkan, et le driver voulu pour cet essai ('$label').
       Appliquer et redémarrer si l'app le demande (relancez-la ensuite, logcat continue).
    2. Lancer le jeu de test, jouer 2 à 3 minutes (passer au moins un chargement).
    3. Noter le FPS de l'overlay, puis quitter le jeu proprement.
  Appuyez sur Entrée ici quand c'est fait (ou si l'app a crashé).
MSG
  read -r _

  "$ADB" logcat -d -v threadtime > "$out/logcat-full.txt" 2>/dev/null || true
  grep -E 'hook_impl|adrenotools|RPCSX-UI|AndroidRuntime|DEBUG|libc |vulkan|Vulkan|VK_ERROR|DEVICE_LOST|tu_|freedreno|kgsl|lowmemorykiller|am_kill|XR readiness|driver identity' \
    "$out/logcat-full.txt" > "$out/logcat-filtered.txt" || true

  local core_log; core_log="$(find_core_log)"
  "$ADB" pull "$core_log" "$out/ARMSX3.log" >/dev/null 2>&1 || log "ARMSX3.log introuvable ($core_log) ; définissez ARMSX3_DATA_ROOT"
  "$ADB" pull "/sdcard/Android/data/$PKG/files/logs" "$out/app-logs" >/dev/null 2>&1 || true

  {
    echo "label        : $label"
    echo "date         : $(date -Iseconds 2>/dev/null || date)"
    echo "appareil     : $(prop ro.product.model) / Android $(prop ro.build.version.release) / build $(prop ro.build.display.id)"
    echo
    echo "--- driver (ARMSX3.log) ---"
    grep -hE 'Found Vulkan-compatible GPU|Vulkan driver identity|custom Vulkan driver|custom driver|Custom driver|setCustomDriver' "$out/ARMSX3.log" 2>/dev/null | tail -8 || echo "(rien)"
    echo
    echo "--- driver (logcat hook_impl / RPCSX-UI) ---"
    grep -hE 'hook_impl|Loading custom driver|could not be loaded|cannot load' "$out/logcat-filtered.txt" | tail -8 || echo "(rien)"
    echo
    echo "--- XR readiness ---"
    grep -h 'XR readiness' "$out/ARMSX3.log" 2>/dev/null || echo "(absent: core sans le patch XR readiness, ou device jamais créé)"
    echo
    echo "--- erreurs GPU / crash ---"
    grep -hE 'VK_ERROR|DEVICE_LOST|device lost|Device lost|OUT_OF_DEVICE_MEMORY|Fatal signal|SIGSEGV|SIGABRT|tu_device|tu_bo|kgsl.*(fault|hang)|lowmemorykiller|Killing' \
      "$out/ARMSX3.log" "$out/logcat-filtered.txt" 2>/dev/null | sort -u | head -20 || echo "(aucune)"
    echo
    echo "--- renderer ---"
    grep -hE 'Renderer:|renderer =|Resolution|resolution_scale|Frame rate' "$out/ARMSX3.log" 2>/dev/null | head -6 || true
    echo
    echo "FPS observé (overlay) : ______   Jeu : ______________   Remarques : ______________"
  } > "$out/summary.txt"

  log "résumé :"
  sed 's/^/    /' "$out/summary.txt"
}

verdict_for() {
  local dir="$1" ident ahb err
  ident="$(grep -h 'Vulkan driver identity' "$dir/ARMSX3.log" 2>/dev/null | tail -1 | sed -E "s/.*identity: '([^']*)'.*/\1/")"
  [[ -n "$ident" ]] || ident="?"
  if grep -qh 'AHardwareBuffer swapchain import is AVAILABLE' "$dir/ARMSX3.log" 2>/dev/null; then ahb="oui"
  elif grep -qh 'AHardwareBuffer swapchain import is NOT' "$dir/ARMSX3.log" 2>/dev/null; then ahb="NON"
  else ahb="?"; fi
  if grep -qhE 'VK_ERROR|DEVICE_LOST|Fatal signal|OUT_OF_DEVICE_MEMORY' "$dir/ARMSX3.log" "$dir/logcat-filtered.txt" 2>/dev/null; then err="ECHEC"; else err="ok"; fi
  printf '%-18s | %-28s | AHB %-3s | %s\n' "$(basename "$dir")" "$ident" "$ahb" "$err"
}

cmd_summary() {
  [[ -d "$RESULTS_DIR" ]] || die "pas de résultats dans $RESULTS_DIR"
  printf '%-18s | %-28s | %-7s | %s\n' "essai" "driver en service" "AHB" "rendu"
  printf '%s\n' "-------------------+------------------------------+---------+-------"
  for d in "$RESULTS_DIR"/*/; do verdict_for "${d%/}"; done
  echo
  echo "Lecture : 'driver en service' doit nommer turnip pour un essai Turnip (sinon le chargement a"
  echo "échoué et c'est le driver Qualcomm qui a tourné). 'AHB oui' = ce driver peut servir un backend"
  echo "OpenXR. 'ECHEC' = voir results/<essai>/summary.txt."
}

case "${1:-}" in
  info) cmd_info ;;
  install) cmd_install "${2:-}" ;;
  push-drivers) cmd_push_drivers ;;
  run) cmd_run "${2:-}" ;;
  summary) cmd_summary ;;
  *) sed -n '2,30p' "$0"; exit 1 ;;
esac
