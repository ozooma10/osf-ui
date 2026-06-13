// Galactic Almanac dataset.
//
// Shipped as a SCRIPT that assigns a global, NOT a JSON file fetched at runtime.
// Rationale: the view is loaded as file:///index.html and network/filesystem are
// denied by the sandbox, so a fetch("data.json") would be blocked. A <script>
// include is served by the local SandboxFileSystem and works identically in a
// plain browser (standalone testing) and in-game.
//
// This is a hand-curated SAMPLE. The real almanac is meant to be GENERATED from
// the game's own records (planet/biome/resource forms) using the workspace
// extraction tools (Tools\xEdit / BAE, or OSF RE for deeper pulls) and written
// out to this same shape. Keep the field names stable; main.js sorts/filters on
// them and the JSON Schema-style contract below documents one body.
//
//   body := {
//     system:      string   -- star system name
//     name:        string   -- body (planet/moon/asteroid) name
//     type:        "Rock" | "Ice" | "Gas Giant" | "Asteroid" | "Moon"
//     parent:      string|null  -- parent body if this is a moon, else null
//     gravity:     number   -- surface gravity, g
//     tempClass:   string   -- "Frozen" | "Cold" | "Temperate" | "Hot" | "Inferno"
//     tempC:       number   -- representative surface temp, deg C
//     atmosphere:  string   -- composition + density, e.g. "O2, Std"
//     magnetosphere: string -- "None" | "Weak" | "Average" | "Strong" | ...
//     water:       number   -- surface water, %
//     flora:       number   -- catalogued flora species (0 = none)
//     fauna:       number   -- catalogued fauna species (0 = none)
//     landable:    boolean  -- can the player land here
//     habitable:   boolean  -- suitable for an outpost / habitat
//     traits:      string[] -- planet traits (may be empty)
//     resources:   string[] -- harvestable resources, by element symbol / id
//     poi:         string[] -- notable locations (may be empty)
//   }

"use strict";

window.ALMANAC = {
  // A small legend so resource chips can show a friendly name on hover.
  resourceNames: {
    "Fe": "Iron", "Al": "Aluminum", "He-3": "Helium-3", "H2O": "Water",
    "U": "Uranium", "Pb": "Lead", "Co": "Cobalt", "Ni": "Nickel",
    "Cl": "Chlorine", "Ar": "Argon", "Be": "Beryllium", "Ir": "Iridium",
    "Cu": "Copper", "W": "Tungsten", "V": "Vanadium", "Au": "Gold",
    "F": "Fluorine", "Ne": "Neon", "Ti": "Titanium"
  },

  bodies: [
    {
      system: "Alpha Centauri", name: "Jemison", type: "Rock", parent: null,
      gravity: 1.00, tempClass: "Temperate", tempC: 18, atmosphere: "O2, Std",
      magnetosphere: "Average", water: 70, flora: 39, fauna: 41,
      landable: true, habitable: true, traits: ["Aurorae"],
      resources: ["Fe", "Al", "H2O", "Co", "Ni", "Cl"],
      poi: ["New Atlantis", "MAST District"]
    },
    {
      system: "Alpha Centauri", name: "Gagarin", type: "Rock", parent: null,
      gravity: 0.79, tempClass: "Temperate", tempC: 11, atmosphere: "O2, Std",
      magnetosphere: "Weak", water: 55, flora: 22, fauna: 18,
      landable: true, habitable: true, traits: [],
      resources: ["Fe", "Al", "Cu", "H2O", "Cl", "W"],
      poi: ["Gagarin Landing"]
    },
    {
      system: "Sol", name: "Earth", type: "Rock", parent: null,
      gravity: 1.00, tempClass: "Temperate", tempC: 14, atmosphere: "None",
      magnetosphere: "None", water: 0, flora: 0, fauna: 0,
      landable: true, habitable: false, traits: ["Magnetic Hum"],
      resources: ["Fe", "Al", "Ni", "Cl", "Pb"],
      poi: ["NASA Launch Tower"]
    },
    {
      system: "Sol", name: "Mars", type: "Rock", parent: null,
      gravity: 0.38, tempClass: "Cold", tempC: -63, atmosphere: "CO2, Thin",
      magnetosphere: "None", water: 0, flora: 0, fauna: 0,
      landable: true, habitable: false, traits: [],
      resources: ["Fe", "Al", "Ni", "H2O", "Be"],
      poi: ["Cydonia"]
    },
    {
      system: "Sol", name: "Luna", type: "Moon", parent: "Earth",
      gravity: 0.17, tempClass: "Cold", tempC: -53, atmosphere: "None",
      magnetosphere: "None", water: 0, flora: 0, fauna: 0,
      landable: true, habitable: false, traits: [],
      resources: ["Fe", "Al", "He-3", "Ti"],
      poi: []
    },
    {
      system: "Sol", name: "Jupiter", type: "Gas Giant", parent: null,
      gravity: 2.53, tempClass: "Frozen", tempC: -145, atmosphere: "Hydrogen",
      magnetosphere: "Strong", water: 0, flora: 0, fauna: 0,
      landable: false, habitable: false, traits: ["Gravitational Anomaly"],
      resources: ["He-3", "H2O", "Ne"],
      poi: []
    },
    {
      system: "Cheyenne", name: "Akila", type: "Rock", parent: null,
      gravity: 1.07, tempClass: "Temperate", tempC: 22, atmosphere: "O2, Std",
      magnetosphere: "Average", water: 50, flora: 26, fauna: 33,
      landable: true, habitable: true, traits: ["Frequent Sandstorms"],
      resources: ["Fe", "Al", "H2O", "Cl", "U", "V"],
      poi: ["Akila City", "GalBank HQ"]
    },
    {
      system: "Volii", name: "Volii Alpha", type: "Rock", parent: null,
      gravity: 0.92, tempClass: "Temperate", tempC: 16, atmosphere: "O2, Std",
      magnetosphere: "Average", water: 95, flora: 31, fauna: 47,
      landable: true, habitable: true, traits: ["Aurorae"],
      resources: ["H2O", "Cl", "F", "Ir", "Au"],
      poi: ["Neon", "Ryujin Tower"]
    },
    {
      system: "Narion", name: "Vectera", type: "Moon", parent: "Anselon",
      gravity: 0.30, tempClass: "Cold", tempC: -28, atmosphere: "CO2, Thin",
      magnetosphere: "Weak", water: 10, flora: 6, fauna: 0,
      landable: true, habitable: false, traits: [],
      resources: ["Fe", "Al", "H2O", "Ar", "He-3"],
      poi: ["Argos Extractors Mining Outpost"]
    },
    {
      system: "Narion", name: "Anselon", type: "Gas Giant", parent: null,
      gravity: 1.90, tempClass: "Frozen", tempC: -120, atmosphere: "Hydrogen",
      magnetosphere: "Strong", water: 0, flora: 0, fauna: 0,
      landable: false, habitable: false, traits: [],
      resources: ["He-3", "Ne", "H2O"],
      poi: []
    },
    {
      system: "Cheyenne", name: "Montara Luna", type: "Moon", parent: "Akila",
      gravity: 0.22, tempClass: "Cold", tempC: -40, atmosphere: "None",
      magnetosphere: "None", water: 0, flora: 4, fauna: 0,
      landable: true, habitable: false, traits: ["Coronal Mass Ejections"],
      resources: ["Fe", "Al", "He-3", "Pb", "U"],
      poi: ["Deserted Bunker"]
    },
    {
      system: "Alpha Centauri", name: "Olympus", type: "Ice", parent: null,
      gravity: 0.64, tempClass: "Frozen", tempC: -88, atmosphere: "CO2, Thin",
      magnetosphere: "Weak", water: 20, flora: 9, fauna: 3,
      landable: true, habitable: false, traits: ["Thermal Vents"],
      resources: ["H2O", "Fe", "Be", "Cl", "Ar"],
      poi: []
    }
  ]
};
