/*
 * testes_extra.c - Testes adicionais para o sistema de arquivos sofs.
 *
 * Este programa realiza testes em cenários não cobertos pelo exemplo.c básico:
 *   1. Prevenção de loop cíclico de softlinks (limite de 5 indireções).
 *   2. Limite da tabela global de arquivos abertos (máximo de 10 handles).
 *   3. Persistência de dados de hardlinks após exclusão do arquivo original.
 *   4. Gravação e leitura incremental (avanço automático do ponteiro de posição).
 *
 * Compilação:
 *   gcc -Wall -Wextra -std=c99 -I./include -m32 src/testes_extra.c -o bin/testes_extra -L./lib -lsofs
 *
 * Execução:
 *   ./bin/testes_extra
 */

#include <stdio.h>
#include <string.h>
#include "sofs.h"

static void listar_diretorio(void)
{
    SOFS_DIRENT entrada;
    int ret;

    printf("=== Conteúdo do diretório raiz ===\n");

    if (sofs_opendir() != 0)
    {
        printf("  [erro] sofs_opendir falhou\n");
        return;
    }

    while ((ret = sofs_readdir(&entrada)) == 0)
    {
        printf("  %-30s  tipo=%s  tamanho=%u bytes\n",
               entrada.name,
               entrada.fileType == TYPEVAL_REGULAR ? "regular" : "link",
               (unsigned int)entrada.fileSize);
    }

    sofs_closedir();
    printf("=== Fim do diretório ===\n\n");
}

int main(void)
{
    int particao = 0;
    int setores_por_bloco = 2;

    printf("--- INICIANDO TESTES ADICIONAIS SOFS ---\n\n");

    /* 1. Formatação e Montagem */
    printf("Formatando partição...\n");
    if (sofs_format(particao, setores_por_bloco) != 0)
    {
        fprintf(stderr, "Erro no sofs_format\n");
        return 1;
    }
    printf("Montando partição...\n");
    if (sofs_mount(particao) != 0)
    {
        fprintf(stderr, "Erro no sofs_mount\n");
        return 1;
    }
    printf("Montagem OK\n\n");

    /* =====================================================================
     * Teste A: Gravação e leitura com avanço automático do cursor
     * ===================================================================== */
    printf("[Teste A] Gravação e leitura com avanço de cursor (offsets)...\n");
    {
        SOFS_FILE arq = sofs_create("arq_cursor.txt");
        if (arq < 0)
        {
            printf("  [erro] falha ao criar arquivo\n");
        }
        else
        {
            // Grava os primeiros 5 bytes
            sofs_write(arq, "AAAAA", 5);
            // Grava mais 5 bytes (devem ser gravados na sequência)
            sofs_write(arq, "BBBBB", 5);
            sofs_close(arq);

            // Reabre para leitura parcial e escrita no meio
            arq = sofs_open("arq_cursor.txt");
            char buf[16];
            memset(buf, 0, sizeof(buf));
            
            // Lê os primeiros 5 bytes ("AAAAA")
            int lidos = sofs_read(arq, buf, 5);
            printf("  Lidos primeiro: '%s' (%d bytes)\n", buf, lidos);

            // Grava "CCCCC" a partir da posição atual (5)
            sofs_write(arq, "CCCCC", 5);
            sofs_close(arq);

            // Abre e lê tudo para verificar o conteúdo final
            arq = sofs_open("arq_cursor.txt");
            memset(buf, 0, sizeof(buf));
            lidos = sofs_read(arq, buf, 15);
            printf("  Conteúdo final (esperado 'AAAAACCCCC'): '%s' (%d bytes)\n", buf, lidos);
            sofs_close(arq);
        }
    }
    printf("\n");

    /* =====================================================================
     * Teste B: Limite da tabela de arquivos abertos (max 10)
     * ===================================================================== */
    printf("[Teste B] Limite da tabela de arquivos abertos (max 10)...\n");
    {
        SOFS_FILE handles[12];
        char nome_arq[32];
        int i;

        // Abre 10 arquivos
        printf("  Abrindo 10 arquivos simultaneamente...\n");
        for (i = 0; i < 10; i++)
        {
            sprintf(nome_arq, "temp_%d.txt", i);
            handles[i] = sofs_create(nome_arq);
            if (handles[i] < 0)
                printf("  [erro] Falha ao abrir arquivo %d\n", i);
        }

        // Tenta abrir o 11º
        printf("  Tentando abrir o 11º arquivo (deve falhar)...\n");
        handles[10] = sofs_create("temp_10.txt");
        if (handles[10] < 0)
        {
            printf("  [Sucesso] Abertura do 11º arquivo falhou corretamente (retorno: %d)\n", handles[10]);
        }
        else
        {
            printf("  [erro] Conseguiu abrir o 11º arquivo! Handle: %d\n", handles[10]);
            sofs_close(handles[10]);
        }

        // Fecha o primeiro e tenta abrir novamente um novo
        printf("  Fechando o arquivo 0 e abrindo um novo...\n");
        sofs_close(handles[0]);
        handles[11] = sofs_create("novo_temp.txt");
        if (handles[11] >= 0)
        {
            printf("  [Sucesso] Novo arquivo aberto após liberar vaga na tabela (Handle: %d)\n", handles[11]);
            sofs_close(handles[11]);
        }
        else
        {
            printf("  [erro] Falha ao abrir novo arquivo mesmo após fechar um anterior\n");
        }

        // Limpa os arquivos temporários abertos
        for (i = 1; i < 10; i++)
        {
            sofs_close(handles[i]);
        }
        
        // Remove arquivos temporários
        for (i = 0; i < 10; i++)
        {
            sprintf(nome_arq, "temp_%d.txt", i);
            sofs_delete(nome_arq);
        }
        sofs_delete("novo_temp.txt");
    }
    printf("\n");

    /* =====================================================================
     * Teste C: Prevenção de loop cíclico de softlinks (limite de 5 indireções)
     * ===================================================================== */
    printf("[Teste C] Prevenção de loops cíclicos de softlinks...\n");
    {
        printf("  Criando ciclo de softlinks: link_a -> link_b e link_b -> link_a...\n");
        sofs_sln("link_a", "link_b");
        sofs_sln("link_b", "link_a");

        printf("  Tentando abrir link_a (deve falhar por excesso de indireções)...\n");
        SOFS_FILE h = sofs_open("link_a");
        if (h < 0)
        {
            printf("  [Sucesso] Abertura falhou corretamente para evitar loop infinito (retorno: %d)\n", h);
        }
        else
        {
            printf("  [erro] Conseguiu abrir link cíclico! Handle: %d\n", h);
            sofs_close(h);
        }

        // Limpeza dos links do ciclo
        sofs_delete("link_a");
        sofs_delete("link_b");
    }
    printf("\n");

    /* =====================================================================
     * Teste D: Persistência de Hardlink após exclusão do arquivo original
     * ===================================================================== */
    printf("[Teste D] Persistência de Hardlink após exclusão do original...\n");
    {
        printf("  Criando arquivo 'orig.txt' e gravando dados...\n");
        SOFS_FILE h_orig = sofs_create("orig.txt");
        if (h_orig >= 0)
        {
            sofs_write(h_orig, "Dados do Hardlink", 17);
            sofs_close(h_orig);
        }

        printf("  Criando hardlink 'hlink.txt' -> 'orig.txt'...\n");
        sofs_hln("hlink.txt", "orig.txt");

        printf("  Removendo o arquivo original 'orig.txt'...\n");
        sofs_delete("orig.txt");

        printf("  Tentando ler os dados através do hardlink 'hlink.txt'...\n");
        SOFS_FILE h_link = sofs_open("hlink.txt");
        if (h_link >= 0)
        {
            char buf[32];
            memset(buf, 0, sizeof(buf));
            int n = sofs_read(h_link, buf, sizeof(buf) - 1);
            printf("  Dados lidos de hlink.txt: '%s' (%d bytes)\n", buf, n);
            sofs_close(h_link);

            if (strcmp(buf, "Dados do Hardlink") == 0)
            {
                printf("  [Sucesso] Os dados permanecem acessíveis no hardlink mesmo após remoção do original!\n");
            }
            else
            {
                printf("  [erro] Os dados lidos não correspondem ao esperado.\n");
            }
        }
        else
        {
            printf("  [erro] Falha ao abrir o hardlink após remover o original.\n");
        }

        printf("  Removendo o hardlink 'hlink.txt'...\n");
        sofs_delete("hlink.txt");
    }
    printf("\n");

    /* 5. Listar e Desmontar */
    listar_diretorio();

    printf("Desmontando partição...\n");
    if (sofs_umount() != 0)
    {
        fprintf(stderr, "Erro no sofs_umount\n");
        return 1;
    }
    printf("Desmontagem OK\n\n");

    printf("--- TESTES ADICIONAIS CONCLUÍDOS COM SUCESSO ---\n");
    return 0;
}
