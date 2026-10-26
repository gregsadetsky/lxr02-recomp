scenarios are data: the run script makes a run directory with scenario.txt, card.img
(optional) and a symlink lxr.elf, and the board writes dac1.raw dac2.raw lcd.txt
midiout.txt params.txt there. the firmware image is the same for every scenario.

    # comment
    card cards/kit-zippe.img     (read by the run script only; no card line = no card)
    seed c0ffee                  (hex, rng seed)
    <ms> midi 90 24 7f           (bytes on the din midi input, hex)
    <ms> button <0..39> <1|0>
    <ms> encoder <+/-detents>
    <ms> encbutton <1|0>
    <ms> adc <index> <value>
    <ms> end
