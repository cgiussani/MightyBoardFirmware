(**** This is a build-plate calibration script for a Dual-Head MakerBot Replicator ****)
(**** Do not run this code on any other kind of machine! ****)
G21 (set units to mm)
G90 (set positioning to absolute)

M70 (Attendi...)

(**** begin homing ****)
G162 X Y F2500 (home XY axes maximum)
G161 Z F1100 (home Z axis minimum)
G92 Z-5 (set Z to -5)
G1 Z0.0 (move Z to "0")
G161 Z F100 (home Z axis minimum)
G92 X152 Y75 Z0 
(M132 X Y Z A B (Recall stored home offsets for XYZAB axis)
(**** end homing ****)

M71 (Cerca le manopole   sotto il piano e    stringile di 4 o 5  giri.)
M71 (Sto per muovere gli estrusori in varie  posizioni per le    regolazioni.)
M71 (Per ogni posizione  dovrai regolare la  manopola            corrispondente.)
M71 (Regola il piano in  modo che il         biglietto da visita scorra liberamente) 
M71 (senza troppo attritotra l'ugello e il   piano stesso.)
M70 ( Attendi ...)

M70 ( Attendi ...)
G1 Z5 F3300.0
G1 X16.5 Y-74 (Move to front of plate)
G1 Z0
M71 (Regola le manopole  in modo che il      biglietto da visita scorra liberamente)

M70 ( Attendi ...)
G1 Z5 F3300.0
G1 X16.5 Y60 (Move to back of plate)
G1 Z0
M71 (Regola le manopole  in modo che il      biglietto da visita scorra liberamente)

M70 ( Attendi ...)
G1 Z5 F3300.0
G1 Y0 X106.5
G1 Z0
M71 (Regola le manopole  in modo che il      biglietto da visita scorra liberamente)

M70 ( Attendi ...)
G1 Z5 F3300.0
G1 X-73.5
G1 Z0
M71 (Regola le manopole  in modo che il      biglietto da visita scorra liberamente)

M70 ( Attendi ...)
G1 Z5 F3300.0
G1 Y0 X16.5
G1 Z0
M71 (Ora verifica che il biglietto scorra trail piano e gli      estrusori)
