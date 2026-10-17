# Guida di campo — messa in servizio, collaudo e dispute

Per ingegneri e tecnici che installano il monitor su una baia reale.
Niente numeri inventati: ogni cifra che fattura esce dal vostro journal,
ogni cifra di targa esce dai vostri strumenti. Questa guida dice **come
misurare**, non **quanto vale**.

## 1. Confini — leggeteli prima di avvitare qualsiasi cosa

- Non è un contatore legale (MID/Eichrecht), non è una funzione di sicurezza
  (IEC 61508), non comanda il power stage. Produce il record locale esatto;
  la certificazione è un'altra azienda, la sicurezza resta negli interlock.
- Il journal dimostra **coerenza interna** (nessuna riga modificata senza
  rompere la catena). Non dimostra provenienza: chi riscrive l'intero file
  ricalcola la catena. Se il rischio dispute lo richiede, ancorate (§6).
- `billing_complete == true` è l'unico campo che la fatturazione automatica
  può leggere. Tutto il resto resta in revisione manuale. Mai fatturare
  `energy_lower_bound` / `time_upper_bound` senza revisione.

## 2. Messa in servizio (checklist)

1. **Bus**: verificate bitrate e terminazione con `candump`/`cansend`
   (tipicamente 500 kbit/s su impianti industriali — confermate sul vostro
   schema, non su questa riga). Il monitor non trasmette mai.
2. **DBC del costruttore**: adottate il file OEM, non l'ID demo.
   ```bash
   ./build/charger_monitor --dump-dbc --dbc vendor.dbc
   ./build/charger_monitor --replay --dbc vendor.dbc \
       --voltage-signal DC_Voltage --current-signal DC_Current < frames.txt
   ```
   Regole fatte rispettare all'avvio: segnali di misura a 16 bit unsigned,
   zero-offset, allineati al byte; fattore come scala razionale esatta;
   stato da tabella `VAL_` con almeno due significati distinti. Qualsiasi
   cosa diversa abortisce l'avvio con il motivo — un decode indovinato
   fatturerebbe numeri sbagliati in silenzio.
3. **Configurazione**: generate il template, riempitelo, validatelo.
   ```bash
   ./build/charger_monitor --print-config > bay-07.json
   # editare: asset_id, timeout, tariffa, diagnostica
   ```
   Un campo errato o sconosciuto blocca l'avvio con il percorso del campo:
   una tariffa digitata male non resta mai a zero in silenzio. Verificate
   esplicitamente che `energy_eur_per_kwh` sia la tariffa contrattualizzata:
   **tariffa zero = fattura zero che nessuno nota**.
4. **Soglie diagnostiche**: tutte disabilitate di default. Impostatele dai
   vostri dati elettrici (stuck-at, slew rate V/A, sag sotto carico, hunting)
   con l'ingegnere elettrico. Una diagnostica non blocca mai la carica:
   solleva un warning, incrementa un contatore, appare in
   `snapshot.diagnostics`.
5. **Storage del journal**: filesystem durevole, `fsync` attivo di default.
   Mai `/tmp`, mai NFS senza commit sincrono. Il journal è il registro di
   fatturazione: trattatelo come tale (permessi, backup, rotazione).

## 3. Collaudo di accettazione (dopo ogni installazione e ogni aggiornamento)

```bash
# 1. Replay di una sequenza nota e verifica della catena
./build/charger_monitor --replay --journal bay.journal --replay-interval-ms 1000 < frames.txt
./build/charger_monitor --verify-journal --journal bay.journal   # exit 0 integro, 3 alterato

# 2. Quadratura economica: solo billing_complete si fattura
./build/charger_settle bay.journal --summary

# 3. Prova di una fattura contestata (pacchetto completo in un JSON)
./build/charger_settle bay.journal --dispute bay-07 12

# 4. CDR per il backend (sottoinsieme OCPI, VAT omesso, start derivato e dichiarato)
./build/charger_settle bay.journal --cdr bay-07 12

# 5. Ancora esterna su storage append-only (WORM, bucket object-lock, o stampa archiviata)
./build/charger_monitor --anchor-journal --journal bay.journal --anchor-file /mnt/worm/bay-07.anchor
./build/charger_monitor --verify-anchor --journal bay.journal --anchor-file /mnt/worm/bay-07.anchor

# 6. Vettore canonico: 480 V x 10 A x 1 h = esattamente 4800 Wh (su qualsiasi macchina)
./build/charger_bench        # righe EXACT riproducibili; il throughput è misurato, non promesso
```

Il collaudo passa se e solo se: catena integra, `billing_complete` solo su
sessioni pulite, dispute pack verificabile con strumenti standard (§5),
ancora verificata, bench senza DRIFT sulle righe exact.

## 4. Tariffe orarie (TOU) — per spostare i carichi di notte

```bash
./build/charger_monitor --print-tou > tou.json   # template charger.tou.v1
./build/charger_settle bay.journal --check-tou tou.json        # valida copertura 24 h
./build/charger_settle bay.journal --tou-settle tou.json --summary
```

- Ore in `HH:MM` **UTC**; `end < start` attraversa la mezzanotte.
- Ogni secondo del giorno deve essere coperto da una finestra o dal fallback:
  un buco viene **rifiutato** (exit 2), mai prezzato a zero. Zero è la
  direzione peggiore in cui sbagliare su un canale di fatturazione.
- La colonna `tou_estimate_eur` è una **stima a potenza costante** per
  decidere lo shifting notturno, non la fattura: il journal non conserva la
  serie temporale intra-sessione. Il totale esatto del ledger resta l'unico
  record fatturabile. Sessioni con `closed_at_utc` non interpretabile sono
  elencate come `refused`, non stimate.
- Le tariffe nei file `charger_project/examples/tou/` sono **segnaposto** per
  esercitare il percorso: sostituitele con la tariffa contrattualizzata del
  fornitore prima di qualsiasi analisi.

## 5. Procedura di dispute — rispondere a "perché 5,30 EUR?"

1. Riesportate il pacchetto: `charger_settle bay.journal --dispute ASSET ID`.
   Contiene contatori, tariffa di record, calcolo e provenienza della catena
   (`open`/`close`, `close_hash`, digest del file).
2. Riverificate senza il nostro software (consegnate queste due righe al cliente):
   ```bash
   sha256sum bay.journal
   awk -F'\t' 'NR==<close_line>{print $1"\t"substr($0,index($0,$3))}' bay.journal | sha256sum
   ```
   Il secondo digest deve eguagliare `close_hash` nel pacchetto: la catena è
   `SHA256(prev_hash TAB resto)`, ricalcolabile con `cut` e `sha256sum`.
3. Ricalcolate il prezzo dal blocco tariffario incorporato
   (`energy_cost + time_cost + flat_cost == total_cost`, interi in micro-EUR).
4. Se esiste un'ancora (§6), verificate il prefisso: una riscrittura totale
   del file non sopravvive all'ancora anche se la catena interna torna.

## 6. Ancora esterna — chiudere il buco di non-ripudio

La catena vive nello stesso file che protegge. L'ancora ne estrae lo stato
(`sha256` del file, `last_hash`, numero righe, timestamp UTC) in un file
separato da custodire su storage append-only:

- Scrivete l'ancora a fine turno / fine giornata dal sistema di supervisione.
- Conservate le ancore in ordine: ognuna impegna il prefisso fino alla sua riga.
- `verify-anchor` dimostra che il prefisso non è stato riscritto **e** lascia
  crescere il journal: le righe aggiunte dopo l'ancora sono riportate
  (`appended`), non rifiutate — la catena prova il suffisso, l'ancora il prefisso.
- L'ancora non è una firma: non attribuisce chi ha scritto. Se serve
  attribuzione (firma operatore / PKI di sito), firmate il file ancora con la
  vostra infrastruttura esistente — il formato è JSON stabile e testo.

## 7. Economia — le formule, con i vostri numeri

Nessuna tariffa è scritta in questa guida: i vostri prezzi stanno nel file di
configurazione e nel blocco tariffario di ogni riga di chiusura. Le formule:

- **Ricavo fatturabile** (per sessione `billing_complete`):
  `ceil(energia_Wh, step) × prezzo_energia + ceil(tempo_s, step) × prezzo_tempo + flat`,
  con arrotondamento OCPI a multiplo superiore e clamp min/max opzionali.
  Il report `charger_settle --summary` lo scompone in ENERGY / TIME / FLAT.
- **Perdita da tariffa zero**: `energia_kWh × prezzo_kWh` per ogni sessione
  fatturata a tariffa non configurata. Il guardrail è l'avvio che rifiuta
  configurazioni ambigue — verificate comunque la tariffa a ogni deploy.
- **Costo di una dispute**: `ore_tecnico × costo_orario + accrediti_riconosciuti
  + rischio_churn`. Il pacchetto `--dispute` sposta quasi tutto il primo
  termine da "ore" a "minuti": misurate le vostre ore prima e dopo, quella è
  la metrica che giustifica il progetto.
- **Disponibilità / utilizzo**: dal portfolio (`--portfolio`), in ppm con
  `-1 = indefinito`, mai zero fittizio. Confrontate baie tra loro sullo
  stesso journal, non su finestre diverse.
- **Throughput**: misuratelo in loco con `charger_bench` in Release. Riferimento
  indicativo su macchina di sviluppo: ~1,1–1,5 M frame/s contro ~3 500 frame/s
  di un bus CAN a 500 kbit/s e 1 Hz — l'esattezza è il vincolo, non la velocità.
  Citare il rapporto, non la cifra.

## 8. Errori visti sul campo (e come evitarli)

| Sintomo | Causa probabile | Azione |
| --- | --- | --- |
| Fatture a zero | Tariffa non impostata / config non caricata | Avvio con `--config` versionato, check del blocco tariffario nel dispute pack |
| Sessioni sempre `held` | Timeout troppo stretti o bus disturbato | Allargate `measurement_timeout_ms`/`status_timeout_ms` dai dati reali; bonifica EMC |
| `MEASUREMENT_GAP` frequenti | Buchi di comunicazione | Mai interpolare: la sessione è `lower_bound` e va in revisione |
| Orfani dopo il riavvio | Processo morto a sessione aperta | Energia non fatturabile per costruzione; procedura di revisione manuale |
| TOU rifiutato | Buco di copertura o finestre sovrapposte | `--check-tou`, aggiungere fallback o correggere gli orari |
| Ancora non verifica | Prefisso riscritto o journal troncato | Bloccare la fatturazione, indagare, non rigenerare l'ancora sul file alterato |

## 9. Manutenzione

- Rieseguite il collaudo (§3) dopo ogni aggiornamento software e ogni cambio
  configurazione. La CI di progetto gira su build/test/e2e locali: replicate
  almeno `ctest`, `e2e_replay.sh` e `charger_demo` prima di toccare l'hardware.
- Ruotate i journal per periodo (mensile) e ancorate la chiusura di ogni
  periodo. Un journal da un anno intero è verificabile ma scomodo in dispute.
- Conservate config + DBC + ancore insieme al journal: tra tre anni la
  domanda sarà "con quale tariffa e quale decode fu misurata", e la risposta
  deve stare nell'archivio, non nella memoria di qualcuno.
