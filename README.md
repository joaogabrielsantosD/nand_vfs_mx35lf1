# MX35LF1 NAND Flash Driver (ESP-IDF)

Driver em C para o **NAND Flash MX35LF1GE4AB** da **Macronix**, desenvolvido para uso com **ESP32** utilizando o framework **ESP-IDF** e comunicação via **SPI**.

Esta biblioteca fornece uma interface simples para inicialização, leitura, escrita e apagamento de memória NAND, abstraindo os comandos de baixo nível do dispositivo.

---

## 📦 Dispositivo Suportado

- **Fabricante:** Macronix International Co., Ltd.
- **Part Number:** MX35LF1GE4AB-Z4I-TR
- **Capacidade:** 1 Gbit (128MB) NAND Flash
- **Interface:** SPI
- **Tensão:** 3.3 V
- **Datasheet:**  
  https://mouser.com/datasheet/2/819/MX35LF1GE4AB_2c_3V_2c_1Gb_2c_v1_9-3371019.pdf

---

## 🧩 Estrutura dos Arquivos

- [MX35LF1.c](src/MX35LF1.c) → Implementação do driver
- [MX35LF1.h](include/MX35LF1.h) → Interface pública e tipos
- [MX35LF1_Registers.h](include/MX35LF1_Registers.h) → Definições de comandos e registradores

---

## 🚀 Funcionalidades

- Inicialização e desinicialização do dispositivo
- Leitura de páginas NAND
- Escrita de páginas NAND
- Apagamento de blocos
- Apagamento completo (bulk erase)
- Controle de:
  - Write Enable / Write Protect
  - HOLD e WP via GPIO
- Comunicação SPI usando `spi_device_polling_transmit`
- Suporte a SPI2 ou SPI3 (configurável via `sdkconfig`)

---

## ⚠️ Restrições Importantes

> **O bloco 0 da memória NAND NÃO deve ser utilizado.**

O **bloco 0** é reservado internamente pelo dispositivo e/ou pode conter informações críticas como:

- Dados de inicialização interna
- Tabelas internas do fabricante
- Áreas suscetíveis a blocos defeituosos

⚠️ **Qualquer tentativa de leitura, escrita ou apagamento do bloco 0 pode resultar em comportamento indefinido ou falha permanente do dispositivo.**

➡️ **Sempre inicie o uso da memória a partir do bloco 1.**

---

## Exemplo

Para mais informações de Funcionalidades e de uso da biblioteca, tome como referência o exemplo ([example/main/main.c](example/main/main.c)).

## 🔧 Configuração de Hardware

### Pinos SPI

A configuração dos pinos é feita via estrutura `nand_mx35_spi_pins_t`:

```c
typedef struct
{
    gpio_num_t mosi_io;
    gpio_num_t miso_io;
    gpio_num_t sclk_io;
    gpio_num_t cs_io;

    gpio_num_t hd_io;
    gpio_num_t wp_io;

} nand_mx35_spi_pins_t;
```

### Configuração do Dispositivo

```c
typedef struct nand_mx35_dev_t
{
    nand_mx35_spi_pins_t spi_pins;

} nand_mx35_config_t;
```

## ⚙️ Configuração do SPI

- Frequência SPI configurada para **50 MHz**
- Host SPI definido via `menuconfig`:
  - `CONFIG_NAND_MX35_SPI2_BUS`
  - `CONFIG_NAND_MX35_SPI3_BUS`

## 📚 API Pública

- **Inicialização**

```c
mx35_err_t nand_mx35_init(const nand_mx35_config_t *cfg);
```

Inicializa o barramento SPI, configura GPIOs e prepara o dispositivo.

- **Desinicialização**

```c
mx35_err_t nand_mx35_deinit(void);
```

libera recursos do SPI.

- **Leitura de Página**

```c
mx35_err_t nand_mx35_read_page(
    uint16_t block,
    uint8_t page,
    uint8_t *buffer,
    size_t len,
    uint16_t *page_address
);
```

📌 Nota: o parâmetro block deve ser ≥ 1.

- **Escrita de Página**

```c
mx35_err_t nand_mx35_write_page(
    uint16_t block,
    uint8_t page,
    uint8_t *buffer,
    size_t len,
    uint16_t *page_address
);
```

📌 Nota: o parâmetro block deve ser ≥ 1.

- **Apagamento de Bloco**

```c
mx35_err_t nand_mx35_erase_block(uint16_t block);
```

📌 Nota: o parâmetro block deve ser ≥ 1.

- **Apagamento Completo**

```c
mx35_err_t nand_mx35_bulk_erase(void);
```

⚠️ Operação destrutiva – apaga todo o conteúdo da memória, exceto restrições internas do dispositivo.

## ❌ Códigos de Retorno

```c
#define MX35_OK               0
#define MX35_FAIL             1
#define MX35_READ_FAIL        2
#define MX35_WRITE_FAIL       3
#define MX35_INVALID_ARGUMENT 4
#define MX35_NO_MEM           5
#define MX35_INVALID_BLOCK    6
#define MX35_ERASE_FAIL       7
```

## 🧠 Utilidades

Funções para conversão entre bloco e página:

```c
uint8_t mx35_PageAddress_to_Page(uint16_t page_address);
uint16_t mx35_PageAddress_to_Block(uint16_t page_address);
uint16_t mx35_PageAddress(uint16_t block, uint8_t page);
```

Funções para adquirir os dados de Blocos Corrompidos>

```c
mx35_err_t mx35_get_bad_block_array(uint8_t *buf, size_t len);
uint8_t mx35_get_bad_block_count();
```
