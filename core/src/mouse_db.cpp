#include "fit/mouse_db.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>

namespace fit {

const std::vector<KnownMouse>& known_mice() {
    // Specs are from the maker's product page on the date given. Leave a field blank (0 or "") when the
    // page does not state it; never fill it in from memory.
    static const std::vector<KnownMouse> mice = {
        {"Logitech", "G Pro X Superlight 2", "HERO 2", 100, 44000, {}, 8000,
         "The spec table says 32,000 DPI; the page says up to 44,000 with current firmware.",
         "logitechg.com/en-us/products/gaming-mice/pro-x2-superlight-wireless-mouse.html", "2026-10-29"},
        {"Logitech", "G Pro X Superlight", "HERO", 100, 25600, {}, 1000, "",
         "logitechg.com/en-us/products/gaming-mice/pro-x-superlight-wireless-mouse.html", "2026-10-29"},
        {"Logitech", "G305 Lightspeed", "HERO", 200, 12000, {}, 1000, "",
         "logitechg.com/en-us/products/gaming-mice/g305-lightspeed-wireless-gaming-mouse.html", "2026-10-29"},
        {"Logitech", "G502 HERO", "HERO", 100, 25600, {}, 1000, "",
         "logitechg.com/en-us/products/gaming-mice/g502-hero-gaming-mouse.html", "2026-10-29"},
        {"Razer", "Viper V3 Pro", "Focus Pro 35K Gen-2", 0, 35000, {}, 8000, "8000 Hz with Razer's HyperPolling technology.",
         "razer.com/gaming-mice/razer-viper-v3-pro", "2026-10-29"},
        {"Razer", "DeathAdder V3 Pro", "Focus Pro 30K", 0, 30000, {}, 8000,
         "8000 Hz with the HyperPolling Wireless Dongle, sold separately.", "razer.com/gaming-mice/razer-deathadder-v3-pro",
         "2026-10-29"},
        {"Razer", "Viper V2 Pro", "Focus Pro 30K", 0, 30000, {}, 8000,
         "8000 Hz with the HyperPolling Wireless Dongle, sold separately.", "razer.com/gaming-mice/razer-viper-v2-pro",
         "2026-10-29"},
        {"Razer", "Basilisk V3", "Focus+ 26K", 0, 26000, {}, 0, "", "razer.com/gaming-mice/razer-basilisk-v3", "2026-10-29"},
        {"Zowie", "EC2-DW", "", 0, 0, {400, 800, 1000, 1200, 1600, 3200}, 4000, "",
         "zowie.benq.com/en-us/mouse/ec2-dw.html", "2026-10-29"},
        {"SteelSeries", "Aerox 3 Wireless", "", 0, 18000, {}, 0, "", "steelseries.com/gaming-mice/aerox-3-2022", "2026-10-29"},
        // Checked on each product page on 2026-10-29.
        {"Logitech", "G502 X Plus", "HERO 44K", 100, 44000, {}, 1000, "One version on the page is listed with 100–25,600 DPI.", "logitechg.com/en-us/shop/p/g502-x-plus-wireless-lightforce.910-006160", "2026-10-29"},
        {"Logitech", "G502 X Lightspeed", "HERO 44K", 100, 44000, {}, 1000, "", "logitechg.com/en-us/shop/p/g502-x-wireless-lightforce.910-006187", "2026-10-29"},
        {"Logitech", "G502 X", "HERO 25K", 100, 25600, {}, 0, "", "logitechg.com/en-us/shop/p/g502-x-wired-lightforce.910-006144", "2026-10-29"},
        {"Logitech", "G Pro 2 Lightspeed", "HERO 2", 100, 44000, {}, 1000, "", "logitechg.com/en-us/shop/p/pro-2-lightspeed.910-007290", "2026-10-29"},
        {"Logitech", "G502 Lightspeed", "HERO 25K", 100, 25600, {}, 1000, "", "logitechg.com/en-us/shop/p/g502-lightspeed-wireless-gaming-mouse.910-005565", "2026-10-29"},
        {"Logitech", "G903 Lightspeed", "HERO 25K", 0, 0, {}, 1000, "", "logitechg.com/en-us/shop/p/g903-hero-wireless-gaming-mouse.910-005670", "2026-10-29"},
        {"Logitech", "G309 Lightspeed", "HERO 25K", 100, 25600, {}, 0, "", "logitechg.com/en-us/shop/p/g309-lightspeed-gaming-mouse.910-007205", "2026-10-29"},
        {"Razer", "DeathAdder V3", "Focus Pro 30K", 0, 30000, {}, 8000, "", "razer.com/gaming-mice/razer-deathadder-v3", "2026-10-29"},
        {"Razer", "Viper V3 HyperSpeed", "Focus Pro 30K", 0, 30000, {}, 8000, "8000 Hz with HyperPolling wireless.", "razer.com/gaming-mice/razer-viper-v3-hyperspeed", "2026-10-29"},
        {"Razer", "DeathAdder Essential", "", 0, 6400, {}, 0, "", "razer.com/gaming-mice/razer-deathadder-essential", "2026-10-29"},
        {"Razer", "Cobra", "", 0, 8500, {}, 0, "", "razer.com/gaming-mice/razer-cobra", "2026-10-29"},
        {"Razer", "Basilisk V3 Pro 35K", "", 0, 0, {}, 8000, "8000 Hz with the Mouse Dock Pro or HyperPolling Wireless Dongle (both sold separately).", "razer.com/gaming-mice/razer-basilisk-v3-pro-35k", "2026-10-29"},
        {"Zowie", "EC1-DW", "", 0, 0, {400, 800, 1000, 1200, 1600, 3200}, 4000, "", "zowie.benq.com/en-us/mouse/ec1-dw.html", "2026-10-29"},
        {"Zowie", "EC3-DW", "", 0, 0, {400, 800, 1000, 1200, 1600, 3200}, 4000, "", "zowie.benq.com/en-us/mouse/ec3-dw.html", "2026-10-29"},
        {"Zowie", "FK1-DW", "", 0, 0, {400, 800, 1000, 1200, 1600, 3200}, 4000, "", "zowie.benq.com/en-us/mouse/fk1-dw.html", "2026-10-29"},
        {"Zowie", "FK2-DW", "", 0, 0, {400, 800, 1000, 1200, 1600, 3200}, 4000, "", "zowie.benq.com/en-us/mouse/fk2-dw.html", "2026-10-29"},
        {"Zowie", "ZA12-DW", "", 0, 0, {400, 800, 1000, 1200, 1600, 3200}, 4000, "", "zowie.benq.com/en-us/mouse/za12-dw.html", "2026-10-29"},
        {"Zowie", "ZA13-DW", "", 0, 0, {400, 800, 1000, 1200, 1600, 3200}, 4000, "", "zowie.benq.com/en-us/mouse/za13-dw.html", "2026-10-29"},
        {"Zowie", "S2-DW", "", 0, 0, {400, 800, 1000, 1200, 1600, 3200}, 4000, "", "zowie.benq.com/en-us/mouse/s2-dw.html", "2026-10-29"},
        {"Zowie", "U2-DW", "", 0, 0, {400, 800, 1000, 1200, 1600, 3200}, 4000, "", "zowie.benq.com/en-us/mouse/u2-dw.html", "2026-10-29"},
        // Listed on the maker's own product list on 2026-10-29; specs not checked yet (all blank).
        {"Logitech", "G Pro X3 Superstrike", "", 0, 0, {}, 0, "", "logitechg.com/en-us/shop/c/gaming-mice", "2026-10-29"},
        {"Logitech", "G Pro X2 Superstrike", "", 0, 0, {}, 0, "", "logitechg.com/en-us/shop/c/gaming-mice", "2026-10-29"},
        {"Logitech", "G305 X Lightspeed", "", 0, 0, {}, 0, "", "logitechg.com/en-us/shop/c/gaming-mice", "2026-10-29"},
        {"Logitech", "G305 X Superlight", "", 0, 0, {}, 0, "", "logitechg.com/en-us/shop/c/gaming-mice", "2026-10-29"},
        {"Logitech", "G Pro X Superlight 2c", "", 0, 0, {}, 0, "", "logitechg.com/en-us/shop/c/gaming-mice", "2026-10-29"},
        {"Logitech", "G Pro X Superlight 2 DEX", "", 0, 0, {}, 0, "", "logitechg.com/en-us/shop/c/gaming-mice", "2026-10-29"},
        {"Logitech", "G Pro X Superlight 2 SE", "", 0, 0, {}, 0, "", "logitechg.com/en-us/shop/c/gaming-mice", "2026-10-29"},
        {"Razer", "Viper V4 Pro", "", 0, 0, {}, 0, "", "razer.com/pc/gaming-mice", "2026-10-29"},
        {"Razer", "Viper V3 Pro SE", "", 0, 0, {}, 0, "", "razer.com/pc/gaming-mice", "2026-10-29"},
        {"Razer", "DeathAdder V4 Pro", "", 0, 0, {}, 0, "", "razer.com/pc/gaming-mice", "2026-10-29"},
        {"Razer", "DeathAdder V3 HyperSpeed", "", 0, 0, {}, 0, "", "razer.com/pc/gaming-mice", "2026-10-29"},
        {"Razer", "Basilisk V4 Pro", "", 0, 0, {}, 0, "", "razer.com/pc/gaming-mice", "2026-10-29"},
        {"Razer", "Basilisk V4 HyperSpeed", "", 0, 0, {}, 0, "", "razer.com/pc/gaming-mice", "2026-10-29"},
        {"Razer", "Basilisk V3 35K", "", 0, 0, {}, 0, "", "razer.com/pc/gaming-mice", "2026-10-29"},
        {"Razer", "Basilisk V3 X HyperSpeed", "", 0, 0, {}, 0, "", "razer.com/pc/gaming-mice", "2026-10-29"},
        {"Razer", "Basilisk Mobile", "", 0, 0, {}, 0, "", "razer.com/pc/gaming-mice", "2026-10-29"},
        {"Razer", "Cobra Pro", "", 0, 0, {}, 0, "", "razer.com/pc/gaming-mice", "2026-10-29"},
        {"Razer", "Cobra HyperSpeed", "", 0, 0, {}, 0, "", "razer.com/pc/gaming-mice", "2026-10-29"},
        {"Razer", "Naga V3 Pro", "", 0, 0, {}, 0, "", "razer.com/pc/gaming-mice", "2026-10-29"},
        {"Razer", "Naga V2 HyperSpeed", "", 0, 0, {}, 0, "", "razer.com/pc/gaming-mice", "2026-10-29"},
        {"Razer", "Naga Left-Handed Edition", "", 0, 0, {}, 0, "", "razer.com/pc/gaming-mice", "2026-10-29"},
        {"Razer", "Orochi V2", "", 0, 0, {}, 0, "", "razer.com/pc/gaming-mice", "2026-10-29"},
        {"Razer", "Viper Mini Signature Edition", "", 0, 0, {}, 0, "", "razer.com/pc/gaming-mice", "2026-10-29"},
        {"Razer", "Boomslang 20th Anniversary Edition", "", 0, 0, {}, 0, "", "razer.com/pc/gaming-mice", "2026-10-29"},
        {"SteelSeries", "Aerox 3 Wireless Gen 2", "", 0, 0, {}, 0, "", "steelseries.com/gaming-mice", "2026-10-29"},
        {"SteelSeries", "Sensei Pro", "", 0, 0, {}, 0, "", "steelseries.com/gaming-mice", "2026-10-29"},
        {"SteelSeries", "Sensei Pro Mini", "", 0, 0, {}, 0, "", "steelseries.com/gaming-mice", "2026-10-29"},
        {"SteelSeries", "Rival Pro", "", 0, 0, {}, 0, "", "steelseries.com/gaming-mice", "2026-10-29"},
        {"SteelSeries", "Rival Pro Mini", "", 0, 0, {}, 0, "", "steelseries.com/gaming-mice", "2026-10-29"},
        {"SteelSeries", "Aerox 9 Wireless", "", 0, 0, {}, 0, "", "steelseries.com/gaming-mice", "2026-10-29"},
        {"SteelSeries", "Aerox 5 Wireless", "", 0, 0, {}, 0, "", "steelseries.com/gaming-mice", "2026-10-29"},
        {"SteelSeries", "Aerox 5", "", 0, 0, {}, 0, "", "steelseries.com/gaming-mice", "2026-10-29"},
        {"SteelSeries", "Aerox 3", "", 0, 0, {}, 0, "", "steelseries.com/gaming-mice", "2026-10-29"},
        {"SteelSeries", "Rival 3 Gen 2 Wireless", "", 0, 0, {}, 0, "", "steelseries.com/gaming-mice", "2026-10-29"},
        {"SteelSeries", "Rival 3 Gen 2", "", 0, 0, {}, 0, "", "steelseries.com/gaming-mice", "2026-10-29"},
        {"SteelSeries", "Rival 3 Wireless", "", 0, 0, {}, 0, "", "steelseries.com/gaming-mice", "2026-10-29"},
        {"SteelSeries", "Rival 5", "", 0, 0, {}, 0, "", "steelseries.com/gaming-mice", "2026-10-29"},
        {"SteelSeries", "Prime", "", 0, 0, {}, 0, "", "steelseries.com/gaming-mice", "2026-10-29"},
        {"Corsair", "Sabre v3 Pro", "", 0, 0, {}, 0, "", "corsair.com/us/en/s/gaming-mice-comparison", "2026-10-29"},
        {"Corsair", "Sabre v3 Pro PureVector", "", 0, 0, {}, 0, "", "corsair.com/us/en/s/gaming-mice-comparison", "2026-10-29"},
        {"Corsair", "Sabre v3 Pro PureVector Ultralight Ergo", "", 0, 0, {}, 0, "", "corsair.com/us/en/s/gaming-mice-comparison", "2026-10-29"},
        {"Corsair", "Sabre v3 Pro PureVector MG", "", 0, 0, {}, 0, "", "corsair.com/us/en/s/gaming-mice-comparison", "2026-10-29"},
        {"Corsair", "Sabre v3 Pro PureVector CF Ergo", "", 0, 0, {}, 0, "", "corsair.com/us/en/s/gaming-mice-comparison", "2026-10-29"},
        {"Corsair", "Sabre v2 Pro Wireless Magnesium Alloy", "", 0, 0, {}, 0, "", "corsair.com/us/en/s/gaming-mice-comparison", "2026-10-29"},
        {"Corsair", "Sabre v2 Pro Carbon Fiber Wireless", "", 0, 0, {}, 0, "", "corsair.com/us/en/s/gaming-mice-comparison", "2026-10-29"},
        {"Corsair", "Sabre v2 Pro Ultralight", "", 0, 0, {}, 0, "", "corsair.com/us/en/s/gaming-mice-comparison", "2026-10-29"},
        {"Corsair", "Nightsword v2 Wireless", "", 0, 0, {}, 0, "", "corsair.com/us/en/s/gaming-mice-comparison", "2026-10-29"},
        {"Corsair", "Nightsabre RGB Wireless", "", 0, 0, {}, 0, "", "corsair.com/us/en/s/gaming-mice-comparison", "2026-10-29"},
        {"Corsair", "Scimitar Elite Wireless", "", 0, 0, {}, 0, "", "corsair.com/us/en/s/gaming-mice-comparison", "2026-10-29"},
        {"Corsair", "Scimitar Elite Wireless SE", "", 0, 0, {}, 0, "", "corsair.com/us/en/s/gaming-mice-comparison", "2026-10-29"},
        {"Corsair", "M75 Wireless", "", 0, 0, {}, 0, "", "corsair.com/us/en/s/gaming-mice-comparison", "2026-10-29"},
        {"Corsair", "M75 Air Wireless", "", 0, 0, {}, 0, "", "corsair.com/us/en/s/gaming-mice-comparison", "2026-10-29"},
        {"Corsair", "M55 Wireless", "", 0, 0, {}, 0, "", "corsair.com/us/en/s/gaming-mice-comparison", "2026-10-29"},
        {"Corsair", "Darkstar Wireless RGB", "", 0, 0, {}, 0, "", "corsair.com/us/en/s/gaming-mice-comparison", "2026-10-29"},
        {"Corsair", "Ironclaw RGB Wireless", "", 0, 0, {}, 0, "", "corsair.com/us/en/s/gaming-mice-comparison", "2026-10-29"},
        {"Corsair", "Katar Elite Wireless", "", 0, 0, {}, 0, "", "corsair.com/us/en/s/gaming-mice-comparison", "2026-10-29"},
        {"Corsair", "Katar Pro XT", "", 0, 0, {}, 0, "", "corsair.com/us/en/s/gaming-mice-comparison", "2026-10-29"},
        {"Corsair", "M65 RGB Ultra", "", 0, 0, {}, 0, "", "corsair.com/us/en/s/gaming-mice-comparison", "2026-10-29"},
        {"HyperX", "Pulsefire Haste 3 Pro", "", 0, 0, {}, 0, "", "hyperx.com/collections/gaming-mice", "2026-10-29"},
        {"HyperX", "Pulsefire Haste 3 Wireless", "", 0, 0, {}, 0, "", "hyperx.com/collections/gaming-mice", "2026-10-29"},
        {"HyperX", "Pulsefire Haste 3", "", 0, 0, {}, 0, "", "hyperx.com/collections/gaming-mice", "2026-10-29"},
        {"HyperX", "Pulsefire Haste 2 S Wireless", "", 0, 0, {}, 0, "", "hyperx.com/collections/gaming-mice", "2026-10-29"},
        {"HyperX", "Pulsefire Haste 2 Wireless", "", 0, 0, {}, 0, "", "hyperx.com/collections/gaming-mice", "2026-10-29"},
        {"HyperX", "Pulsefire Haste 2 Mini", "", 0, 0, {}, 0, "", "hyperx.com/collections/gaming-mice", "2026-10-29"},
        {"HyperX", "Pulsefire Haste 2 Pro 4K", "", 0, 0, {}, 0, "", "hyperx.com/collections/gaming-mice", "2026-10-29"},
        {"HyperX", "Pulsefire Haste 2 Core", "", 0, 0, {}, 0, "", "hyperx.com/collections/gaming-mice", "2026-10-29"},
        {"HyperX", "Pulsefire Haste 2", "", 0, 0, {}, 0, "", "hyperx.com/collections/gaming-mice", "2026-10-29"},
        {"HyperX", "Pulsefire Fuse", "", 0, 0, {}, 0, "", "hyperx.com/collections/gaming-mice", "2026-10-29"},
        {"HyperX", "Pulsefire Saga", "", 0, 0, {}, 0, "", "hyperx.com/collections/gaming-mice", "2026-10-29"},
        {"HyperX", "Pulsefire Saga Pro", "", 0, 0, {}, 0, "", "hyperx.com/collections/gaming-mice", "2026-10-29"},
        {"Glorious", "Model O Eternal", "", 0, 0, {}, 0, "", "gloriousgaming.com/collections/mice", "2026-10-29"},
        {"Glorious", "Model O3 Wireless", "", 0, 0, {}, 0, "", "gloriousgaming.com/collections/mice", "2026-10-29"},
        {"Glorious", "Model O3", "", 0, 0, {}, 0, "", "gloriousgaming.com/collections/mice", "2026-10-29"},
        {"Glorious", "Model O Classic Wireless", "", 0, 0, {}, 0, "", "gloriousgaming.com/collections/mice", "2026-10-29"},
        {"Glorious", "Model D3 Wireless", "", 0, 0, {}, 0, "", "gloriousgaming.com/collections/mice", "2026-10-29"},
        {"Glorious", "Model D3", "", 0, 0, {}, 0, "", "gloriousgaming.com/collections/mice", "2026-10-29"},
        {"Glorious", "Model O 2 Wireless", "", 0, 0, {}, 0, "", "gloriousgaming.com/collections/mice", "2026-10-29"},
        {"Glorious", "Model O 2", "", 0, 0, {}, 0, "", "gloriousgaming.com/collections/mice", "2026-10-29"},
        {"Glorious", "Model D 2 Wireless", "", 0, 0, {}, 0, "", "gloriousgaming.com/collections/mice", "2026-10-29"},
        {"Glorious", "Model D 2", "", 0, 0, {}, 0, "", "gloriousgaming.com/collections/mice", "2026-10-29"},
        {"Glorious", "Model I 2 Wireless", "", 0, 0, {}, 0, "", "gloriousgaming.com/collections/mice", "2026-10-29"},
        {"Glorious", "Model I 2", "", 0, 0, {}, 0, "", "gloriousgaming.com/collections/mice", "2026-10-29"},
        {"Glorious", "Model O 2 Mini Wireless", "", 0, 0, {}, 0, "", "gloriousgaming.com/collections/mice", "2026-10-29"},
        {"Glorious", "Model O 2 Mini", "", 0, 0, {}, 0, "", "gloriousgaming.com/collections/mice", "2026-10-29"},
        {"Glorious", "Series 2 Pro Wireless", "", 0, 0, {}, 0, "", "gloriousgaming.com/collections/mice", "2026-10-29"},
        {"Glorious", "Model O 2 Pro Wireless", "", 0, 0, {}, 0, "", "gloriousgaming.com/collections/mice", "2026-10-29"},
        {"Glorious", "Model O 2 Pro Wireless 4K/8KHz Edition", "", 0, 0, {}, 0, "", "gloriousgaming.com/collections/mice", "2026-10-29"},
        {"Glorious", "Model D 2 Pro Wireless", "", 0, 0, {}, 0, "", "gloriousgaming.com/collections/mice", "2026-10-29"},
        {"Glorious", "Model D 2 Pro Wireless 4K/8KHz Edition", "", 0, 0, {}, 0, "", "gloriousgaming.com/collections/mice", "2026-10-29"},
        {"Pulsar", "X2 v3", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "X2 v3 Mini", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "X2H v3", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "X2H v3 Mini", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "X2A v3", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "X2A v3 Mini", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "Xlite v4", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "Xlite v4 Mini", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "Xlite v4 Large", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "X3", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "X3 Mini", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "X3 LHD", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "X2 CrazyLight", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "X2 CrazyLight Mini", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "X2H CrazyLight", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "X2H CrazyLight Mini", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "X2N CrazyLight", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "X2N CrazyLight Mini", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "X3 CrazyLight", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "X3 CrazyLight Mini", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "Xlite CrazyLight", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "Xlite CrazyLight Mini", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "X2F", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "eS FS-1", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "Feinmann F01", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "ZywOo The Chosen Mouse", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "TenZ Signature Edition", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "Susanto-X", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "X2 v2", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "X2 v2 Mini", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "X2H", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "X2H Mini", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "X2A", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "X2A Mini", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "Xlite v3", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "Xlite v3 Mini", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "Xlite v3 Large", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "X2 Wired", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "X2H Wired", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "X2A Wired", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "Xlite Wired", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "X2 v1", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Pulsar", "Xlite v2", "", 0, 0, {}, 0, "", "pulsar.gg/collections/mice", "2026-10-29"},
        {"Lamzu", "Maya X V2", "", 0, 0, {}, 0, "", "lamzu.com", "2026-10-29"},
        {"Lamzu", "Maya S V2", "", 0, 0, {}, 0, "", "lamzu.com", "2026-10-29"},
        {"Lamzu", "Maya X", "", 0, 0, {}, 0, "", "lamzu.com", "2026-10-29"},
        {"Lamzu", "Maya 4K", "", 0, 0, {}, 0, "", "lamzu.com", "2026-10-29"},
        {"Lamzu", "Maya", "", 0, 0, {}, 0, "", "lamzu.com", "2026-10-29"},
        {"Lamzu", "Orcus", "", 0, 0, {}, 0, "", "lamzu.com", "2026-10-29"},
        {"Lamzu", "Thorn V2", "", 0, 0, {}, 0, "", "lamzu.com", "2026-10-29"},
        {"Lamzu", "Thorn 4K", "", 0, 0, {}, 0, "", "lamzu.com", "2026-10-29"},
        {"Lamzu", "Thorn", "", 0, 0, {}, 0, "", "lamzu.com", "2026-10-29"},
        {"Lamzu", "Paro Aurora", "", 0, 0, {}, 0, "", "lamzu.com", "2026-10-29"},
        {"Lamzu", "Paro", "", 0, 0, {}, 0, "", "lamzu.com", "2026-10-29"},
        {"Lamzu", "Inca", "", 0, 0, {}, 0, "", "lamzu.com", "2026-10-29"},
        {"Lamzu", "Tachi", "", 0, 0, {}, 0, "", "lamzu.com", "2026-10-29"},
        {"Lamzu", "Atlantis Mini Pro", "", 0, 0, {}, 0, "", "lamzu.com", "2026-10-29"},
        {"Lamzu", "Atlantis Mini 4K", "", 0, 0, {}, 0, "", "lamzu.com", "2026-10-29"},
        {"Lamzu", "Atlantis OG V2 Pro", "", 0, 0, {}, 0, "", "lamzu.com", "2026-10-29"},
        {"Lamzu", "Atlantis OG V2 4K", "", 0, 0, {}, 0, "", "lamzu.com", "2026-10-29"},
        {"Finalmouse", "UltralightX", "", 0, 0, {}, 0, "", "finalmouse.com", "2026-10-29"},
        {"Finalmouse", "ULX Pro Series", "", 0, 0, {}, 0, "", "finalmouse.com", "2026-10-29"},
        {"Finalmouse", "SLX", "", 0, 0, {}, 0, "", "finalmouse.com", "2026-10-29"},
        {"Finalmouse", "Starlight-12", "", 0, 0, {}, 0, "", "finalmouse.com", "2026-10-29"},
        {"Finalmouse", "Starlight Pro", "", 0, 0, {}, 0, "", "finalmouse.com", "2026-10-29"},
        {"Finalmouse", "Ultralight 2", "", 0, 0, {}, 0, "", "finalmouse.com", "2026-10-29"},
        {"Finalmouse", "Ultralight Pro", "", 0, 0, {}, 0, "", "finalmouse.com", "2026-10-29"},
        {"Finalmouse", "Air58 Ninja", "", 0, 0, {}, 0, "", "finalmouse.com", "2026-10-29"},
        {"Finalmouse", "Tournament Pro", "", 0, 0, {}, 0, "", "finalmouse.com", "2026-10-29"},
        {"Finalmouse", "Scream One", "", 0, 0, {}, 0, "", "finalmouse.com", "2026-10-29"},
        {"Endgame Gear", "OP1w 4k v2", "", 0, 0, {}, 0, "", "endgamegear.com", "2026-10-29"},
        {"Endgame Gear", "OP1we", "", 0, 0, {}, 0, "", "endgamegear.com", "2026-10-29"},
        {"Endgame Gear", "OP1 8k v2", "", 0, 0, {}, 0, "", "endgamegear.com", "2026-10-29"},
        {"Endgame Gear", "OP1 RGB", "", 0, 0, {}, 0, "", "endgamegear.com", "2026-10-29"},
        {"Endgame Gear", "XM2w 4k v2", "", 0, 0, {}, 0, "", "endgamegear.com", "2026-10-29"},
        {"Endgame Gear", "XM2we", "", 0, 0, {}, 0, "", "endgamegear.com", "2026-10-29"},
        {"Endgame Gear", "XM2 8k v2", "", 0, 0, {}, 0, "", "endgamegear.com", "2026-10-29"},
        {"ASUS ROG", "Gladius IV Ace", "", 0, 0, {}, 0, "", "rog.asus.com/us/mice-mouse-pads/mice-series/", "2026-10-29"},
        {"ASUS ROG", "Gladius III Core", "", 0, 0, {}, 0, "", "rog.asus.com/us/mice-mouse-pads/mice-series/", "2026-10-29"},
        {"ASUS ROG", "Spatha X 65K", "", 0, 0, {}, 0, "", "rog.asus.com/us/mice-mouse-pads/mice-series/", "2026-10-29"},
        {"ASUS ROG", "Harpe II Ace", "", 0, 0, {}, 0, "", "rog.asus.com/us/mice-mouse-pads/mice-series/", "2026-10-29"},
        {"ASUS ROG", "Harpe II Extreme Edition 20", "", 0, 0, {}, 0, "", "rog.asus.com/us/mice-mouse-pads/mice-series/", "2026-10-29"},
        {"ASUS ROG", "Harpe Ace Extreme", "", 0, 0, {}, 0, "", "rog.asus.com/us/mice-mouse-pads/mice-series/", "2026-10-29"},
        {"ASUS ROG", "Harpe Mini Core", "", 0, 0, {}, 0, "", "rog.asus.com/us/mice-mouse-pads/mice-series/", "2026-10-29"},
        {"ASUS ROG", "Keris II Origin", "", 0, 0, {}, 0, "", "rog.asus.com/us/mice-mouse-pads/mice-series/", "2026-10-29"},
        {"ASUS ROG", "Chakram", "", 0, 0, {}, 0, "", "rog.asus.com/us/mice-mouse-pads/mice-series/", "2026-10-29"},
    };
    return mice;
}

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::vector<std::string> words(const std::string& s) {
    std::istringstream is(lower(s));
    std::vector<std::string> w;
    for (std::string x; is >> x;) w.push_back(x);
    return w;
}

std::string squash(const std::string& s) {
    std::string out;
    for (const auto& w : words(s)) out += (out.empty() ? "" : " ") + w;
    return out;
}

std::string with_commas(int v) {
    std::string s = std::to_string(v);
    for (int i = int(s.size()) - 3; i > 0; i -= 3) s.insert(size_t(i), ",");
    return s;
}

}  // namespace

std::vector<const KnownMouse*> search_mice(const std::string& query, size_t max) {
    std::vector<const KnownMouse*> out;
    const auto q = words(query);
    if (q.empty()) return out;
    for (const auto& m : known_mice()) {
        const std::string name = lower(m.full_name());
        bool all = true;
        for (const auto& w : q) all = all && name.find(w) != std::string::npos;
        if (all) out.push_back(&m);
        if (out.size() >= max) break;
    }
    return out;
}

const KnownMouse* find_mouse(const std::string& name) {
    const std::string n = squash(name);
    if (n.empty()) return nullptr;
    for (const auto& m : known_mice())
        if (squash(m.full_name()) == n) return &m;
    return nullptr;
}

MouseAdvice mouse_advice(const KnownMouse& m, int dpi, double measured_hz) {
    MouseAdvice a;
    if (dpi > 0) {
        if (!m.dpi_steps.empty()) {
            if (std::find(m.dpi_steps.begin(), m.dpi_steps.end(), dpi) == m.dpi_steps.end()) {
                std::string steps;
                for (int s : m.dpi_steps) steps += (steps.empty() ? "" : ", ") + std::to_string(s);
                a.warnings.push_back("The " + m.model + " offers only " + steps + " DPI, so " + std::to_string(dpi) +
                                     " is probably not your setting. Check the DPI on your mouse.");
            }
        } else if (m.max_dpi > 0 && dpi > m.max_dpi) {
            a.warnings.push_back(std::to_string(dpi) + " DPI is more than the " + m.model + " can do (up to " +
                                 with_commas(m.max_dpi) + "). Check the DPI in your mouse software.");
        } else if (m.min_dpi > 0 && dpi < m.min_dpi) {
            a.warnings.push_back(std::to_string(dpi) + " DPI is less than the " + m.model + " can do (from " +
                                 std::to_string(m.min_dpi) + "). Check the DPI in your mouse software.");
        }
    }
    if (measured_hz > 0.0 && m.max_polling_hz > 0) {
        char buf[240];
        if (measured_hz > m.max_polling_hz * 1.3) {
            std::snprintf(buf, sizeof buf,
                          "FitSuite measured about %.0f reports a second, more than the %s's %d Hz: is this the mouse you "
                          "measured with?",
                          measured_hz, m.model.c_str(), m.max_polling_hz);
            a.warnings.push_back(buf);
        } else if (measured_hz < m.max_polling_hz * 0.6) {
            std::snprintf(buf, sizeof buf,
                          "Your mouse sends about %.0f reports a second; the %s can do up to %d Hz in its software. "
                          "Either works for the fix; measure again if you change it.",
                          measured_hz, m.model.c_str(), m.max_polling_hz);
            a.notes.push_back(buf);
        }
    }
    return a;
}

}  // namespace fit
