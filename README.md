# Sistema de Monitoramento Ambiental com ESP32, SCD30, MQTT, MySQL e Grafana

Este projeto realiza o monitoramento ambiental de uma sala utilizando um ESP32 conectado a um sensor Sensirion SCD30. O sistema coleta dados de CO₂, temperatura e umidade, envia essas medições via MQTT para um broker local, armazena os dados em um banco MySQL e permite visualização em tempo real pelo Grafana.

## Visão Geral do Sistema

O fluxo principal do sistema é:

SCD30 → ESP32 → MQTT Broker → Backend Python → MySQL → Grafana
