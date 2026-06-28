/*
 * sofs.c - Implementação (esqueleto) do sistema de arquivos sofs.
 *
 * A camada de blocos (sofs-block) é usada para todos os acessos ao disco;
 * a camada de bitmap (bitmap2) gerencia o controle de blocos e i-nodes livres.
 *
 * Layout do sistema de arquivos dentro de uma partição (em ordem):
 *   [bloco 0]          superbloco
 *   [blocos 1 .. bb]   bitmap de blocos livres   (bb = freeBlocksBitmapSize)
 *   [bb+1 .. bb+bi]    bitmap de i-nodes livres  (bi = freeInodeBitmapSize)
 *   [bb+bi+1 .. ...]   área de i-nodes           (10% dos blocos, arredondado para cima)
 *   [resto]            blocos de dados
 *
 * As funções marcadas com TODO são responsabilidade do grupo.
 * As funções auxiliares alloc_data_block(), free_data_block(),
 * alloc_inode() e free_inode() são fornecidas como blocos de construção.
 */

#include <string.h>
#include "sofs.h"
#include "sofs-block.h"

/* -------------------------------------------------------------------------
 * Estado interno de montagem
 * ---------------------------------------------------------------------- */

static int g_mounted = false;
static struct sofs_superbloco g_superbloco;
static unsigned int g_superbloco_sector; /* setor absoluto do superbloco */

/* -------------------------------------------------------------------------
 * Auxiliar: lê o MBR e localiza a partição <partition>.
 * Preenche *first_sector e *num_sectors.
 * Retorna 0 em caso de sucesso.
 * ---------------------------------------------------------------------- */
static int read_partition_info(int partition,
                               unsigned int *first_sector,
                               unsigned int *num_sectors)
{
    unsigned char mbr_buf[SECTOR_SIZE];
    struct sofs_mbr *mbr;

    if (read_sector(0, mbr_buf) != 0)
        return -1;

    mbr = (struct sofs_mbr *)mbr_buf;

    if (partition < 0 || partition >= (int)mbr->numPartitions)
        return -1;

    *first_sector = mbr->partitionTable[partition].firstSector;
    *num_sectors = mbr->partitionTable[partition].lastSector - mbr->partitionTable[partition].firstSector + 1;
    return 0;
}

/* -------------------------------------------------------------------------
 * Funções básicas de criação/destruição de blocos de dados e i-nodes.
 *
 * Fornecidas como blocos de construção para a implementação do grupo em
 * sofs_create, sofs_delete, sofs_read, sofs_write, etc.
 * ---------------------------------------------------------------------- */

/*
 * alloc_data_block - aloca o primeiro bloco de dados livre.
 *
 * Pesquisa no bitmap de dados o primeiro bit livre, marca-o como ocupado,
 * zera o conteúdo do bloco e retorna o número absoluto do bloco na partição.
 *
 * Retorna o número do bloco (>= 0) em caso de sucesso; -1 em caso de erro
 * ou se o disco estiver cheio.
 */
static int alloc_data_block(void)
{
    int bit;
    unsigned int block_size;
    unsigned char *buf;

    if (!g_mounted)
        return -1;

    bit = searchBitmap2(BITMAP_DADOS, 0);
    if (bit < 0)
        return -1;

    if (setBitmap2(BITMAP_DADOS, bit, 1) != 0)
        return -1;

    /* Inicializa o bloco recém-alocado com zeros */
    block_size = g_superbloco.blockSize * SECTOR_SIZE;
    buf = (unsigned char *)__builtin_alloca(block_size);
    memset(buf, 0, block_size);

    /* O primeiro bloco de dados começa após superbloco + bitmaps + área de i-nodes */
    unsigned int first_data_block = 1 + g_superbloco.freeBlocksBitmapSize + g_superbloco.freeInodeBitmapSize + g_superbloco.inodeAreaSize;

    if (write_block(first_data_block + (unsigned int)bit, buf) != 0)
    {
        setBitmap2(BITMAP_DADOS, bit, 0);
        return -1;
    }

    return (int)(first_data_block + (unsigned int)bit);
}

/*
 * free_data_block - libera um bloco de dados previamente alocado.
 *
 *   abs_block_num : número absoluto do bloco na partição (conforme
 *                   retornado por alloc_data_block).
 *
 * Retorna 0 em caso de sucesso; -1 em caso de erro.
 */
static int free_data_block(unsigned int abs_block_num)
{
    unsigned int first_data_block;
    int bit;

    if (!g_mounted)
        return -1;

    first_data_block = 1 + g_superbloco.freeBlocksBitmapSize + g_superbloco.freeInodeBitmapSize + g_superbloco.inodeAreaSize;

    if (abs_block_num < first_data_block)
        return -1;

    bit = (int)(abs_block_num - first_data_block);
    return setBitmap2(BITMAP_DADOS, bit, 0);
}

/*
 * alloc_inode - aloca o primeiro i-node livre.
 *
 * Pesquisa no bitmap de i-nodes o primeiro bit livre, marca-o como ocupado,
 * zera o conteúdo do i-node em disco e retorna o número do i-node.
 *
 * Retorna o número do i-node (>= 0) em caso de sucesso; -1 em caso de erro
 * ou se todos os i-nodes estiverem em uso.
 */
static int alloc_inode(void)
{
    int bit;
    unsigned int inode_block;
    unsigned int inodes_per_block;
    unsigned int inode_offset;
    unsigned char *buf;
    unsigned int block_size;

    if (!g_mounted)
        return -1;

    bit = searchBitmap2(BITMAP_INODE, 0);
    if (bit < 0)
        return -1;

    if (setBitmap2(BITMAP_INODE, bit, 1) != 0)
        return -1;

    /* Zera o i-node em disco */
    block_size = g_superbloco.blockSize * SECTOR_SIZE;
    inodes_per_block = block_size / sizeof(struct sofs_inode);
    inode_block = 1 + g_superbloco.freeBlocksBitmapSize + g_superbloco.freeInodeBitmapSize + (unsigned int)bit / inodes_per_block;
    inode_offset = (unsigned int)bit % inodes_per_block;

    buf = (unsigned char *)__builtin_alloca(block_size);
    if (read_block(inode_block, buf) != 0)
    {
        setBitmap2(BITMAP_INODE, bit, 0);
        return -1;
    }

    memset(buf + inode_offset * sizeof(struct sofs_inode), 0,
           sizeof(struct sofs_inode));

    if (write_block(inode_block, buf) != 0)
    {
        setBitmap2(BITMAP_INODE, bit, 0);
        return -1;
    }

    return bit;
}

/*
 * free_inode - libera um i-node previamente alocado.
 *
 *   inode_num : número do i-node (conforme retornado por alloc_inode).
 *
 * Retorna 0 em caso de sucesso; -1 em caso de erro.
 */
static int free_inode(unsigned int inode_num)
{
    if (!g_mounted)
        return -1;

    return setBitmap2(BITMAP_INODE, (int)inode_num, 0);
}

/* -------------------------------------------------------------------------
 * Gerência do sistema de arquivos
 * ---------------------------------------------------------------------- */

int sofs_identify(char *name, int size)
{
    const char *id = "Artur e Gabriel";
    if (name == NULL || size <= 0)
        return -1;
    strncpy(name, id, size - 1);
    name[size - 1] = '\0';
    return 0;
}

int sofs_format(int partition, int sectors_per_block)
{
    unsigned int first_sector, num_sectors;
    unsigned int num_blocks;
    unsigned int inode_area_blocks;
    unsigned int bitmap_blocks_data;
    unsigned int bitmap_blocks_inode;
    unsigned char block_buf[sectors_per_block * SECTOR_SIZE];
    struct sofs_superbloco *sb;

    if (sectors_per_block <= 0)
        return -1;

    if (read_partition_info(partition, &first_sector, &num_sectors) != 0)
        return -1;

    /* Inicializa a camada de blocos para poder escrever na partição */
    if (init_block_layer(first_sector, (unsigned int)sectors_per_block) != 0)
        return -1;

    num_blocks = num_sectors / (unsigned int)sectors_per_block;

    /* 10% dos blocos para i-nodes, arredondado para cima */
    inode_area_blocks = (num_blocks + 9) / 10;

    /* Um bloco por 8*(sectors_per_block*SECTOR_SIZE) bits necessários em cada bitmap */
    bitmap_blocks_data = (num_blocks + 8 * sectors_per_block * SECTOR_SIZE - 1) / (8 * sectors_per_block * SECTOR_SIZE);
    bitmap_blocks_inode = (inode_area_blocks + 8 * sectors_per_block * SECTOR_SIZE - 1) / (8 * sectors_per_block * SECTOR_SIZE);

    /* Constrói e grava o superbloco (bloco 0 da partição) */
    memset(block_buf, 0, sizeof(block_buf));
    sb = (struct sofs_superbloco *)block_buf;
    memcpy(sb->id, "SOFS", 4);
    sb->version = 0x7E32;
    sb->superblockSize = 1;
    sb->freeBlocksBitmapSize = (WORD)bitmap_blocks_data;
    sb->freeInodeBitmapSize = (WORD)bitmap_blocks_inode;
    sb->inodeAreaSize = (WORD)inode_area_blocks;
    sb->blockSize = (WORD)sectors_per_block;
    sb->diskSize = (DWORD)num_blocks;

    /* Checksum: complemento de um da soma dos 5 primeiros DWORDs */
    {
        DWORD *words = (DWORD *)block_buf;
        DWORD sum = words[0] + words[1] + words[2] + words[3] + words[4];
        sb->Checksum = ~sum;
    }

    if (write_block(0, block_buf) != 0)
        return -1;

    /* TODO: inicializar com zeros as áreas de bitmap e de i-nodes */
    unsigned int block_size = sectors_per_block * SECTOR_SIZE;
    unsigned char *zero_buf = (unsigned char *)__builtin_alloca(block_size);
    memset(zero_buf, 0, block_size);

    for (unsigned int b = 0; b < bitmap_blocks_data; b++)
    {
        if (write_block(1 + b, zero_buf) != 0)
            return -1;
    }

    unsigned char *inode_bitmap_buf = (unsigned char *)__builtin_alloca(block_size);
    memset(inode_bitmap_buf, 0, block_size);
    inode_bitmap_buf[0] = 0x01; /* Reserva o i-node 0 para o diretório raiz */

    if (write_block(1 + bitmap_blocks_data, inode_bitmap_buf) != 0)
        return -1;

    for (unsigned int b = 1; b < bitmap_blocks_inode; b++)
    {
        if (write_block(1 + bitmap_blocks_data + b, zero_buf) != 0)
            return -1;
    }

    unsigned int inode_area_start = 1 + bitmap_blocks_data + bitmap_blocks_inode;
    for (unsigned int b = 0; b < inode_area_blocks; b++)
    {
        if (write_block(inode_area_start + b, zero_buf) != 0)
            return -1;
    }

    return 0;
}

int sofs_mount(int partition)
{
    unsigned int first_sector, num_sectors;
    unsigned char sector_buf[SECTOR_SIZE];
    struct sofs_superbloco *sb;

    if (g_mounted)
        return -1; /* partição já montada */

    if (read_partition_info(partition, &first_sector, &num_sectors) != 0)
        return -1;

    /* Lê o primeiro setor da partição para obter o superbloco */
    if (read_sector(first_sector, sector_buf) != 0)
        return -1;

    sb = (struct sofs_superbloco *)sector_buf;

    /* Valida a assinatura do sistema de arquivos */
    if (memcmp(sb->id, "SOFS", 4) != 0)
        return -1;

    /* Agora sabemos o tamanho do bloco: inicializa a camada de blocos */
    if (init_block_layer(first_sector, (unsigned int)sb->blockSize) != 0)
        return -1;

    /* Abre o subsistema de bitmap */
    g_superbloco_sector = first_sector;
    if (openBitmap2((int)g_superbloco_sector) != 0)
        return -1;

    /* Armazena em cache o superbloco */
    memcpy(&g_superbloco, sb, sizeof(g_superbloco));
    g_mounted = true;
    return 0;
}

int sofs_umount(void)
{
    if (!g_mounted)
        return -1;

    closeBitmap2();
    reset_block_layer();
    memset(&g_superbloco, 0, sizeof(g_superbloco));
    g_mounted = false;
    return 0;
}

/* -------------------------------------------------------------------------
 * Operações de arquivo (TODO)
 * ---------------------------------------------------------------------- */

static struct
{
    int in_use;
    unsigned int inode_num;
    unsigned int pos;
} g_sofs_open_table[10];

SOFS_FILE sofs_create(char *filename)
{
    /* TODO: aloca um i-node (alloc_inode), adiciona um registro de diretório,
     * abre o arquivo e retorna um handle. Se o arquivo já existir,
     * trunca-o para zero bytes primeiro. */
    unsigned int block_size, inode_area, inodes_per_block, records_per_block, ptrs_per_block;
    unsigned int inode_num, lb, ri, i;
    struct sofs_inode root, inode;
    struct sofs_record rec;
    unsigned char *buf, *ibuf;
    int abs_block, new_inode, handle, found;
    unsigned int dir_idx = 0;

    if (!g_mounted || filename == NULL)
        return -1;

    block_size = g_superbloco.blockSize * SECTOR_SIZE;
    inode_area = 1 + g_superbloco.freeBlocksBitmapSize + g_superbloco.freeInodeBitmapSize;
    inodes_per_block = block_size / sizeof(struct sofs_inode);
    records_per_block = block_size / sizeof(struct sofs_record);
    ptrs_per_block = block_size / sizeof(DWORD);
    buf = (unsigned char *)__builtin_alloca(block_size);
    ibuf = (unsigned char *)__builtin_alloca(block_size);

    if (read_block(inode_area, ibuf) != 0)
        return -1;
    memcpy(&root, ibuf, sizeof(root));

    found = 0;
    for (lb = 0; lb < root.blocksFileSize; lb++)
    {
        if (lb < 2)
        {
            if (root.dataPtr[lb] == 0)
                continue;
            abs_block = (int)root.dataPtr[lb];
        }
        else if (lb - 2 < ptrs_per_block)
        {
            if (root.singleIndPtr == 0)
                continue;
            if (read_block(root.singleIndPtr, buf) != 0)
                continue;
            if (((DWORD *)buf)[lb - 2] == 0)
                continue;
            abs_block = (int)((DWORD *)buf)[lb - 2];
        }
        else
            continue;

        if (read_block((unsigned int)abs_block, buf) != 0)
            continue;
        for (ri = 0; ri < records_per_block; ri++)
        {
            struct sofs_record *records = (struct sofs_record *)buf;
            if (records[ri].TypeVal != TYPEVAL_INVALIDO &&
                strcmp(records[ri].name, filename) == 0)
            {
                memcpy(&rec, &records[ri], sizeof(rec));
                found = 1;
                break;
            }
        }
        if (found)
            break;
    }

    if (found)
    {
        unsigned int bi, bj;
        inode_num = rec.inodeNumber;
        if (read_block(inode_area + inode_num / inodes_per_block, ibuf) != 0)
            return -1;
        memcpy(&inode, ibuf + (inode_num % inodes_per_block) * sizeof(struct sofs_inode),
               sizeof(inode));

        for (i = 0; i < 2; i++)
        {
            if (inode.dataPtr[i] != 0)
                free_data_block(inode.dataPtr[i]);
            inode.dataPtr[i] = 0;
        }
        if (inode.singleIndPtr != 0)
        {
            if (read_block(inode.singleIndPtr, buf) == 0)
            {
                DWORD *ptrs = (DWORD *)buf;
                for (bi = 0; bi < ptrs_per_block; bi++)
                    if (ptrs[bi] != 0)
                        free_data_block(ptrs[bi]);
            }
            free_data_block(inode.singleIndPtr);
            inode.singleIndPtr = 0;
        }
        if (inode.doubleIndPtr != 0)
        {
            if (read_block(inode.doubleIndPtr, buf) == 0)
            {
                DWORD *ptrs = (DWORD *)buf;
                for (bi = 0; bi < ptrs_per_block; bi++)
                {
                    if (ptrs[bi] != 0)
                    {
                        if (read_block(ptrs[bi], ibuf) == 0)
                        {
                            DWORD *ptrs2 = (DWORD *)ibuf;
                            for (bj = 0; bj < ptrs_per_block; bj++)
                                if (ptrs2[bj] != 0)
                                    free_data_block(ptrs2[bj]);
                        }
                        free_data_block(ptrs[bi]);
                    }
                }
            }
            free_data_block(inode.doubleIndPtr);
            inode.doubleIndPtr = 0;
        }
        inode.blocksFileSize = 0;
        inode.bytesFileSize = 0;
        if (read_block(inode_area + inode_num / inodes_per_block, ibuf) != 0)
            return -1;
        memcpy(ibuf + (inode_num % inodes_per_block) * sizeof(struct sofs_inode),
               &inode, sizeof(inode));
        if (write_block(inode_area + inode_num / inodes_per_block, ibuf) != 0)
            return -1;

        if (read_block((unsigned int)abs_block, buf) == 0)
        {
            struct sofs_record *records = (struct sofs_record *)buf;
            records[ri].TypeVal = TYPEVAL_REGULAR;
            write_block((unsigned int)abs_block, buf);
        }
    }
    else
    {
        new_inode = alloc_inode();
        if (new_inode < 0)
            return -1;
        inode_num = (unsigned int)new_inode;
        memset(&inode, 0, sizeof(inode));
        inode.RefCounter = 1;
        if (read_block(inode_area + inode_num / inodes_per_block, ibuf) != 0)
        {
            free_inode(inode_num);
            return -1;
        }
        memcpy(ibuf + (inode_num % inodes_per_block) * sizeof(struct sofs_inode),
               &inode, sizeof(inode));
        if (write_block(inode_area + inode_num / inodes_per_block, ibuf) != 0)
        {
            free_inode(inode_num);
            return -1;
        }

        abs_block = -1;
        for (lb = 0; lb < root.blocksFileSize; lb++)
        {
            int db;
            if (lb < 2)
            {
                if (root.dataPtr[lb] == 0)
                    continue;
                db = (int)root.dataPtr[lb];
            }
            else if (lb - 2 < ptrs_per_block)
            {
                if (root.singleIndPtr == 0)
                    continue;
                if (read_block(root.singleIndPtr, buf) != 0)
                    continue;
                if (((DWORD *)buf)[lb - 2] == 0)
                    continue;
                db = (int)((DWORD *)buf)[lb - 2];
            }
            else
                continue;
            if (read_block((unsigned int)db, buf) != 0)
                continue;
            for (ri = 0; ri < records_per_block; ri++)
            {
                if (((struct sofs_record *)buf)[ri].TypeVal == TYPEVAL_INVALIDO)
                {
                    abs_block = db;
                    dir_idx = ri;
                    break;
                }
            }
            if (abs_block >= 0)
                break;
        }

        if (abs_block < 0)
        {
            unsigned int new_lb = root.blocksFileSize;
            if (new_lb < 2)
            {
                abs_block = alloc_data_block();
                if (abs_block < 0)
                {
                    free_inode(inode_num);
                    return -1;
                }
                root.dataPtr[new_lb] = (DWORD)abs_block;
                root.blocksFileSize++;
            }
            else if (new_lb - 2 < ptrs_per_block)
            {
                if (root.singleIndPtr == 0)
                {
                    abs_block = alloc_data_block();
                    if (abs_block < 0)
                    {
                        free_inode(inode_num);
                        return -1;
                    }
                    root.singleIndPtr = (DWORD)abs_block;
                }
                if (read_block(root.singleIndPtr, buf) != 0)
                {
                    free_inode(inode_num);
                    return -1;
                }
                if (((DWORD *)buf)[new_lb - 2] == 0)
                {
                    abs_block = alloc_data_block();
                    if (abs_block < 0)
                    {
                        free_inode(inode_num);
                        return -1;
                    }
                    ((DWORD *)buf)[new_lb - 2] = (DWORD)abs_block;
                    if (write_block(root.singleIndPtr, buf) != 0)
                    {
                        free_inode(inode_num);
                        return -1;
                    }
                }
                else
                {
                    abs_block = (int)((DWORD *)buf)[new_lb - 2];
                }
                root.blocksFileSize++;
            }
            else
            {
                free_inode(inode_num);
                return -1;
            }
            if (read_block(inode_area, ibuf) != 0)
            {
                free_inode(inode_num);
                return -1;
            }
            memcpy(ibuf, &root, sizeof(root));
            if (write_block(inode_area, ibuf) != 0)
            {
                free_inode(inode_num);
                return -1;
            }
            dir_idx = 0;
            if (read_block((unsigned int)abs_block, buf) != 0)
            {
                free_inode(inode_num);
                return -1;
            }
        }

        memset(&((struct sofs_record *)buf)[dir_idx], 0, sizeof(struct sofs_record));
        ((struct sofs_record *)buf)[dir_idx].TypeVal = TYPEVAL_REGULAR;
        strncpy(((struct sofs_record *)buf)[dir_idx].name, filename, 50);
        ((struct sofs_record *)buf)[dir_idx].name[50] = '\0';
        ((struct sofs_record *)buf)[dir_idx].inodeNumber = inode_num;
        if (write_block((unsigned int)abs_block, buf) != 0)
        {
            free_inode(inode_num);
            return -1;
        }
    }

    for (handle = 0; handle < 10; handle++)
    {
        if (!g_sofs_open_table[handle].in_use)
        {
            g_sofs_open_table[handle].in_use = 1;
            g_sofs_open_table[handle].inode_num = inode_num;
            g_sofs_open_table[handle].pos = 0;
            return (SOFS_FILE)handle;
        }
    }
    return -1;
}

int sofs_delete(char *name)
{
    /* TODO: localiza o registro de diretório de <name>, libera todos os blocos
     * de dados referenciados pelo i-node (free_data_block), libera o i-node
     * (free_inode) e invalida o registro de diretório. */
    unsigned int block_size, inode_area, inodes_per_block, records_per_block, ptrs_per_block;
    unsigned int inode_num, lb, ri, i, bi, bj;
    struct sofs_inode root, inode;
    struct sofs_record rec;
    unsigned char *buf, *ibuf;
    int abs_block, found;
    unsigned int dir_block = 0;
    unsigned int dir_idx = 0;

    if (!g_mounted || name == NULL)
        return -1;

    block_size = g_superbloco.blockSize * SECTOR_SIZE;
    inode_area = 1 + g_superbloco.freeBlocksBitmapSize + g_superbloco.freeInodeBitmapSize;
    inodes_per_block = block_size / sizeof(struct sofs_inode);
    records_per_block = block_size / sizeof(struct sofs_record);
    ptrs_per_block = block_size / sizeof(DWORD);
    buf = (unsigned char *)__builtin_alloca(block_size);
    ibuf = (unsigned char *)__builtin_alloca(block_size);

    if (read_block(inode_area, ibuf) != 0)
        return -1;
    memcpy(&root, ibuf, sizeof(root));

    found = 0;
    for (lb = 0; lb < root.blocksFileSize; lb++)
    {
        if (lb < 2)
        {
            if (root.dataPtr[lb] == 0)
                continue;
            abs_block = (int)root.dataPtr[lb];
        }
        else if (lb - 2 < ptrs_per_block)
        {
            if (root.singleIndPtr == 0)
                continue;
            if (read_block(root.singleIndPtr, buf) != 0)
                continue;
            if (((DWORD *)buf)[lb - 2] == 0)
                continue;
            abs_block = (int)((DWORD *)buf)[lb - 2];
        }
        else
            continue;

        if (read_block((unsigned int)abs_block, buf) != 0)
            continue;
        for (ri = 0; ri < records_per_block; ri++)
        {
            struct sofs_record *records = (struct sofs_record *)buf;
            if (records[ri].TypeVal != TYPEVAL_INVALIDO &&
                strcmp(records[ri].name, name) == 0)
            {
                memcpy(&rec, &records[ri], sizeof(rec));
                dir_block = (unsigned int)abs_block;
                dir_idx = ri;
                found = 1;
                break;
            }
        }
        if (found)
            break;
    }
    if (!found)
        return -1;

    inode_num = rec.inodeNumber;
    if (read_block(inode_area + inode_num / inodes_per_block, ibuf) != 0)
        return -1;
    memcpy(&inode, ibuf + (inode_num % inodes_per_block) * sizeof(struct sofs_inode),
           sizeof(inode));

    if (inode.RefCounter <= 1)
    {
        for (i = 0; i < 2; i++)
        {
            if (inode.dataPtr[i] != 0)
                free_data_block(inode.dataPtr[i]);
            inode.dataPtr[i] = 0;
        }
        if (inode.singleIndPtr != 0)
        {
            if (read_block(inode.singleIndPtr, buf) == 0)
            {
                DWORD *ptrs = (DWORD *)buf;
                for (bi = 0; bi < ptrs_per_block; bi++)
                    if (ptrs[bi] != 0)
                        free_data_block(ptrs[bi]);
            }
            free_data_block(inode.singleIndPtr);
            inode.singleIndPtr = 0;
        }
        if (inode.doubleIndPtr != 0)
        {
            if (read_block(inode.doubleIndPtr, buf) == 0)
            {
                DWORD *ptrs = (DWORD *)buf;
                for (bi = 0; bi < ptrs_per_block; bi++)
                {
                    if (ptrs[bi] != 0)
                    {
                        if (read_block(ptrs[bi], ibuf) == 0)
                        {
                            DWORD *ptrs2 = (DWORD *)ibuf;
                            for (bj = 0; bj < ptrs_per_block; bj++)
                                if (ptrs2[bj] != 0)
                                    free_data_block(ptrs2[bj]);
                        }
                        free_data_block(ptrs[bi]);
                    }
                }
            }
            free_data_block(inode.doubleIndPtr);
            inode.doubleIndPtr = 0;
        }

        if (free_inode(inode_num) != 0)
            return -1;
    }
    else
    {
        inode.RefCounter--;
        memcpy(ibuf + (inode_num % inodes_per_block) * sizeof(struct sofs_inode),
               &inode, sizeof(inode));
        if (write_block(inode_area + inode_num / inodes_per_block, ibuf) != 0)
            return -1;
    }

    if (read_block(dir_block, buf) != 0)
        return -1;
    memset(&((struct sofs_record *)buf)[dir_idx], 0, sizeof(struct sofs_record));
    ((struct sofs_record *)buf)[dir_idx].TypeVal = TYPEVAL_INVALIDO;
    return write_block(dir_block, buf);
}

SOFS_FILE sofs_open(char *name)
{
    /* TODO: localiza o registro de diretório de <name>, verifica que o arquivo
     * existe, aloca uma entrada na tabela de arquivos abertos, inicializa o
     * ponteiro de posição em 0 e retorna o handle. */
    unsigned int block_size, inode_area, records_per_block, ptrs_per_block;
    unsigned int inode_num, lb, ri;
    struct sofs_inode root, inode;
    struct sofs_record rec;
    unsigned char *buf, *ibuf;
    int abs_block, found, handle;
    static int g_open_depth = 0;

    if (!g_mounted || name == NULL)
        return -1;

    if (g_open_depth > 5)
        return -1;

    block_size = g_superbloco.blockSize * SECTOR_SIZE;
    inode_area = 1 + g_superbloco.freeBlocksBitmapSize + g_superbloco.freeInodeBitmapSize;
    records_per_block = block_size / sizeof(struct sofs_record);
    ptrs_per_block = block_size / sizeof(DWORD);
    buf = (unsigned char *)__builtin_alloca(block_size);
    ibuf = (unsigned char *)__builtin_alloca(block_size);

    if (read_block(inode_area, ibuf) != 0)
        return -1;
    memcpy(&root, ibuf, sizeof(root));

    found = 0;
    for (lb = 0; lb < root.blocksFileSize; lb++)
    {
        if (lb < 2)
        {
            if (root.dataPtr[lb] == 0)
                continue;
            abs_block = (int)root.dataPtr[lb];
        }
        else if (lb - 2 < ptrs_per_block)
        {
            if (root.singleIndPtr == 0)
                continue;
            if (read_block(root.singleIndPtr, buf) != 0)
                continue;
            if (((DWORD *)buf)[lb - 2] == 0)
                continue;
            abs_block = (int)((DWORD *)buf)[lb - 2];
        }
        else
            continue;

        if (read_block((unsigned int)abs_block, buf) != 0)
            continue;
        for (ri = 0; ri < records_per_block; ri++)
        {
            struct sofs_record *records = (struct sofs_record *)buf;
            if (records[ri].TypeVal != TYPEVAL_INVALIDO &&
                strcmp(records[ri].name, name) == 0)
            {
                memcpy(&rec, &records[ri], sizeof(rec));
                found = 1;
                break;
            }
        }
        if (found)
            break;
    }
    if (!found)
        return -1;

    inode_num = rec.inodeNumber;

    if (rec.TypeVal == TYPEVAL_LINK)
    {
        char target[51];
        if (read_block(inode_area + inode_num / (block_size / sizeof(struct sofs_inode)),
                       ibuf) != 0)
            return -1;
        memcpy(&inode, ibuf + (inode_num % (block_size / sizeof(struct sofs_inode))) * sizeof(struct sofs_inode), sizeof(inode));
        if (inode.dataPtr[0] == 0)
            return -1;
        if (read_block(inode.dataPtr[0], buf) != 0)
            return -1;
        strncpy(target, (char *)buf, 50);
        target[50] = '\0';
        g_open_depth++;
        SOFS_FILE res = sofs_open(target);
        g_open_depth--;
        return res;
    }

    for (handle = 0; handle < 10; handle++)
    {
        if (!g_sofs_open_table[handle].in_use)
        {
            g_sofs_open_table[handle].in_use = 1;
            g_sofs_open_table[handle].inode_num = inode_num;
            g_sofs_open_table[handle].pos = 0;
            return (SOFS_FILE)handle;
        }
    }
    return -1;
}

int sofs_close(SOFS_FILE handle)
{
    /* TODO: valida <handle> e libera sua entrada na tabela de arquivos abertos. */
    if (!g_mounted || handle < 0 || handle >= 10)
        return -1;
    if (!g_sofs_open_table[handle].in_use)
        return -1;
    g_sofs_open_table[handle].in_use = 0;
    return 0;
}

int sofs_read(SOFS_FILE handle, char *buffer, int size)
{
    /* TODO: lê até <size> bytes do arquivo a partir da posição corrente;
     * avança o ponteiro de posição; retorna o número de bytes efetivamente lidos. */
    unsigned int block_size, inode_area, inodes_per_block, ptrs_per_block;
    unsigned int inode_num, pos, file_size, cur_pos, logical_blk, blk_offset, to_read;
    struct sofs_inode inode;
    unsigned char *buf, *ibuf;
    int abs_block, bytes_read;

    if (!g_mounted || handle < 0 || handle >= 10)
        return -1;
    if (!g_sofs_open_table[handle].in_use)
        return -1;
    if (buffer == NULL || size <= 0)
        return 0;

    block_size = g_superbloco.blockSize * SECTOR_SIZE;
    inode_area = 1 + g_superbloco.freeBlocksBitmapSize + g_superbloco.freeInodeBitmapSize;
    inodes_per_block = block_size / sizeof(struct sofs_inode);
    ptrs_per_block = block_size / sizeof(DWORD);
    buf = (unsigned char *)__builtin_alloca(block_size);
    ibuf = (unsigned char *)__builtin_alloca(block_size);

    inode_num = g_sofs_open_table[handle].inode_num;
    if (read_block(inode_area + inode_num / inodes_per_block, ibuf) != 0)
        return -1;
    memcpy(&inode, ibuf + (inode_num % inodes_per_block) * sizeof(struct sofs_inode),
           sizeof(inode));

    pos = g_sofs_open_table[handle].pos;
    file_size = inode.bytesFileSize;
    if (pos >= file_size)
        return 0;
    if (pos + (unsigned int)size > file_size)
        size = (int)(file_size - pos);

    bytes_read = 0;
    while (bytes_read < size)
    {
        cur_pos = pos + (unsigned int)bytes_read;
        logical_blk = cur_pos / block_size;
        blk_offset = cur_pos % block_size;
        to_read = block_size - blk_offset;
        if (to_read > (unsigned int)(size - bytes_read))
            to_read = (unsigned int)(size - bytes_read);

        if (logical_blk < 2)
        {
            if (inode.dataPtr[logical_blk] == 0)
                break;
            abs_block = (int)inode.dataPtr[logical_blk];
        }
        else if (logical_blk - 2 < ptrs_per_block)
        {
            if (inode.singleIndPtr == 0)
                break;
            if (read_block(inode.singleIndPtr, buf) != 0)
                break;
            if (((DWORD *)buf)[logical_blk - 2] == 0)
                break;
            abs_block = (int)((DWORD *)buf)[logical_blk - 2];
        }
        else
        {
            unsigned int lb = logical_blk - 2 - ptrs_per_block;
            unsigned int l1 = lb / ptrs_per_block;
            unsigned int l2 = lb % ptrs_per_block;
            if (inode.doubleIndPtr == 0)
                break;
            if (read_block(inode.doubleIndPtr, buf) != 0)
                break;
            if (((DWORD *)buf)[l1] == 0)
                break;
            if (read_block(((DWORD *)buf)[l1], ibuf) != 0)
                break;
            if (((DWORD *)ibuf)[l2] == 0)
                break;
            abs_block = (int)((DWORD *)ibuf)[l2];
        }

        if (read_block((unsigned int)abs_block, buf) != 0)
            break;
        memcpy(buffer + bytes_read, buf + blk_offset, to_read);
        bytes_read += (int)to_read;
    }

    g_sofs_open_table[handle].pos += (unsigned int)bytes_read;
    return bytes_read;
}

int sofs_write(SOFS_FILE handle, char *buffer, int size)
{
    /* TODO: grava <size> bytes no arquivo a partir da posição corrente,
     * alocando novos blocos de dados conforme necessário (alloc_data_block);
     * avança o ponteiro de posição; retorna o número de bytes gravados. */
    unsigned int block_size, inode_area, inodes_per_block, ptrs_per_block;
    unsigned int inode_num, pos, cur_pos, logical_blk, blk_offset, to_write;
    struct sofs_inode inode;
    unsigned char *buf, *ibuf, *buf2;
    int abs_block, bytes_written, new_block;

    if (!g_mounted || handle < 0 || handle >= 10)
        return -1;
    if (!g_sofs_open_table[handle].in_use)
        return -1;
    if (buffer == NULL || size <= 0)
        return 0;

    block_size = g_superbloco.blockSize * SECTOR_SIZE;
    inode_area = 1 + g_superbloco.freeBlocksBitmapSize + g_superbloco.freeInodeBitmapSize;
    inodes_per_block = block_size / sizeof(struct sofs_inode);
    ptrs_per_block = block_size / sizeof(DWORD);
    buf = (unsigned char *)__builtin_alloca(block_size);
    ibuf = (unsigned char *)__builtin_alloca(block_size);
    buf2 = (unsigned char *)__builtin_alloca(block_size);

    inode_num = g_sofs_open_table[handle].inode_num;
    if (read_block(inode_area + inode_num / inodes_per_block, ibuf) != 0)
        return -1;
    memcpy(&inode, ibuf + (inode_num % inodes_per_block) * sizeof(struct sofs_inode),
           sizeof(inode));

    pos = g_sofs_open_table[handle].pos;
    bytes_written = 0;

    while (bytes_written < size)
    {
        cur_pos = pos + (unsigned int)bytes_written;
        logical_blk = cur_pos / block_size;
        blk_offset = cur_pos % block_size;
        to_write = block_size - blk_offset;
        if (to_write > (unsigned int)(size - bytes_written))
            to_write = (unsigned int)(size - bytes_written);

        abs_block = -1;
        if (logical_blk < 2)
        {
            if (inode.dataPtr[logical_blk] != 0)
                abs_block = (int)inode.dataPtr[logical_blk];
            else
            {
                new_block = alloc_data_block();
                if (new_block < 0)
                    break;
                inode.dataPtr[logical_blk] = (DWORD)new_block;
                inode.blocksFileSize++;
                abs_block = new_block;
                if (read_block(inode_area + inode_num / inodes_per_block, ibuf) != 0)
                    break;
                memcpy(ibuf + (inode_num % inodes_per_block) * sizeof(struct sofs_inode),
                       &inode, sizeof(inode));
                if (write_block(inode_area + inode_num / inodes_per_block, ibuf) != 0)
                    break;
            }
        }
        else if (logical_blk - 2 < ptrs_per_block)
        {
            if (inode.singleIndPtr == 0)
            {
                new_block = alloc_data_block();
                if (new_block < 0)
                    break;
                inode.singleIndPtr = (DWORD)new_block;
                if (read_block(inode_area + inode_num / inodes_per_block, ibuf) != 0)
                    break;
                memcpy(ibuf + (inode_num % inodes_per_block) * sizeof(struct sofs_inode),
                       &inode, sizeof(inode));
                if (write_block(inode_area + inode_num / inodes_per_block, ibuf) != 0)
                    break;
            }
            if (read_block(inode.singleIndPtr, buf) != 0)
                break;
            if (((DWORD *)buf)[logical_blk - 2] != 0)
                abs_block = (int)((DWORD *)buf)[logical_blk - 2];
            else
            {
                new_block = alloc_data_block();
                if (new_block < 0)
                    break;
                ((DWORD *)buf)[logical_blk - 2] = (DWORD)new_block;
                if (write_block(inode.singleIndPtr, buf) != 0)
                    break;
                inode.blocksFileSize++;
                abs_block = new_block;
                if (read_block(inode_area + inode_num / inodes_per_block, ibuf) != 0)
                    break;
                memcpy(ibuf + (inode_num % inodes_per_block) * sizeof(struct sofs_inode),
                       &inode, sizeof(inode));
                if (write_block(inode_area + inode_num / inodes_per_block, ibuf) != 0)
                    break;
            }
        }
        else
        {
            unsigned int lb = logical_blk - 2 - ptrs_per_block;
            unsigned int l1 = lb / ptrs_per_block;
            unsigned int l2 = lb % ptrs_per_block;
            unsigned int l1_block;

            if (inode.doubleIndPtr == 0)
            {
                new_block = alloc_data_block();
                if (new_block < 0)
                    break;
                inode.doubleIndPtr = (DWORD)new_block;
                if (read_block(inode_area + inode_num / inodes_per_block, ibuf) != 0)
                    break;
                memcpy(ibuf + (inode_num % inodes_per_block) * sizeof(struct sofs_inode),
                       &inode, sizeof(inode));
                if (write_block(inode_area + inode_num / inodes_per_block, ibuf) != 0)
                    break;
            }
            if (read_block(inode.doubleIndPtr, buf) != 0)
                break;
            if (((DWORD *)buf)[l1] == 0)
            {
                new_block = alloc_data_block();
                if (new_block < 0)
                    break;
                ((DWORD *)buf)[l1] = (DWORD)new_block;
                if (write_block(inode.doubleIndPtr, buf) != 0)
                    break;
            }
            l1_block = ((DWORD *)buf)[l1];
            if (read_block(l1_block, buf2) != 0)
                break;
            if (((DWORD *)buf2)[l2] != 0)
                abs_block = (int)((DWORD *)buf2)[l2];
            else
            {
                new_block = alloc_data_block();
                if (new_block < 0)
                    break;
                ((DWORD *)buf2)[l2] = (DWORD)new_block;
                if (write_block(l1_block, buf2) != 0)
                    break;
                inode.blocksFileSize++;
                abs_block = new_block;
                if (read_block(inode_area + inode_num / inodes_per_block, ibuf) != 0)
                    break;
                memcpy(ibuf + (inode_num % inodes_per_block) * sizeof(struct sofs_inode),
                       &inode, sizeof(inode));
                if (write_block(inode_area + inode_num / inodes_per_block, ibuf) != 0)
                    break;
            }
        }

        if (abs_block < 0)
            break;

        if (blk_offset != 0 || to_write < block_size)
        {
            if (read_block((unsigned int)abs_block, buf) != 0)
                break;
        }
        else
        {
            memset(buf, 0, block_size);
        }

        memcpy(buf + blk_offset, buffer + bytes_written, to_write);
        if (write_block((unsigned int)abs_block, buf) != 0)
            break;

        bytes_written += (int)to_write;
        if (cur_pos + to_write > inode.bytesFileSize)
        {
            inode.bytesFileSize = cur_pos + to_write;
            if (read_block(inode_area + inode_num / inodes_per_block, ibuf) != 0)
                break;
            memcpy(ibuf + (inode_num % inodes_per_block) * sizeof(struct sofs_inode),
                   &inode, sizeof(inode));
            if (write_block(inode_area + inode_num / inodes_per_block, ibuf) != 0)
                break;
        }
    }

    g_sofs_open_table[handle].pos += (unsigned int)bytes_written;
    return bytes_written;
}

/* -------------------------------------------------------------------------
 * Operações de diretório (TODO)
 * ---------------------------------------------------------------------- */

static unsigned int g_opendir_index = 0;
static int g_opendir_ready = false;

int sofs_opendir(void)
{
    /* TODO: verifica que uma partição está montada, posiciona o ponteiro de
     * entradas no primeiro registro válido do diretório raiz e retorna 0. */
    if (!g_mounted)
        return -1;
    g_opendir_index = 0;
    g_opendir_ready = true;
    return 0;
}

int sofs_readdir(SOFS_DIRENT *dentry)
{
    /* TODO: lê o próximo registro válido do diretório em *dentry e avança o
     * ponteiro de entradas. Retorna valor diferente de zero ao fim do diretório. */
    unsigned int block_size, inode_area, records_per_block, ptrs_per_block;
    struct sofs_inode root;
    unsigned char *buf, *ibuf;
    int abs_block;

    if (!g_mounted || !g_opendir_ready || dentry == NULL)
        return -1;

    block_size = g_superbloco.blockSize * SECTOR_SIZE;
    inode_area = 1 + g_superbloco.freeBlocksBitmapSize + g_superbloco.freeInodeBitmapSize;
    records_per_block = block_size / sizeof(struct sofs_record);
    ptrs_per_block = block_size / sizeof(DWORD);
    buf = (unsigned char *)__builtin_alloca(block_size);
    ibuf = (unsigned char *)__builtin_alloca(block_size);

    if (read_block(inode_area, ibuf) != 0)
        return -1;
    memcpy(&root, ibuf, sizeof(root));

    while (1)
    {
        unsigned int logical_blk = g_opendir_index / records_per_block;
        unsigned int record_idx = g_opendir_index % records_per_block;

        if (logical_blk >= root.blocksFileSize)
        {
            /* Fim do diretório */
            return -1;
        }

        abs_block = -1;
        if (logical_blk < 2)
        {
            if (root.dataPtr[logical_blk] != 0)
            {
                abs_block = (int)root.dataPtr[logical_blk];
            }
        }
        else if (logical_blk - 2 < ptrs_per_block)
        {
            if (root.singleIndPtr != 0)
            {
                if (read_block(root.singleIndPtr, buf) == 0)
                {
                    abs_block = (int)((DWORD *)buf)[logical_blk - 2];
                }
            }
        }

        if (abs_block <= 0)
        {
            g_opendir_index = (logical_blk + 1) * records_per_block;
            continue;
        }

        if (read_block((unsigned int)abs_block, buf) != 0)
        {
            return -1;
        }

        struct sofs_record *records = (struct sofs_record *)buf;

        for (; record_idx < records_per_block; record_idx++, g_opendir_index++)
        {
            if (records[record_idx].TypeVal != TYPEVAL_INVALIDO)
            {
                strncpy(dentry->name, records[record_idx].name, SOFS_MAX_FILE_NAME_SIZE);
                dentry->name[SOFS_MAX_FILE_NAME_SIZE] = '\0';
                dentry->fileType = records[record_idx].TypeVal;

                unsigned int entry_inode_num = records[record_idx].inodeNumber;
                unsigned int inodes_per_block = block_size / sizeof(struct sofs_inode);
                unsigned int entry_inode_block = inode_area + entry_inode_num / inodes_per_block;
                unsigned int entry_inode_offset = entry_inode_num % inodes_per_block;

                unsigned char *entry_ibuf = (unsigned char *)__builtin_alloca(block_size);
                if (read_block(entry_inode_block, entry_ibuf) == 0)
                {
                    struct sofs_inode *entry_inode = (struct sofs_inode *)(entry_ibuf + entry_inode_offset * sizeof(struct sofs_inode));
                    dentry->fileSize = entry_inode->bytesFileSize;
                }
                else
                {
                    dentry->fileSize = 0;
                }

                g_opendir_index++;
                return 0;
            }
        }
    }
}

int sofs_closedir(void)
{
    /* TODO: reinicia o ponteiro de entradas do diretório e retorna 0. */
    if (!g_mounted)
        return -1;
    g_opendir_index = 0;
    g_opendir_ready = false;
    return 0;
}

/* -------------------------------------------------------------------------
 * Operações de link (TODO)
 * ---------------------------------------------------------------------- */

int sofs_sln(char *linkname, char *filename)
{
    /* TODO: cria um softlink chamado <linkname> cujo único bloco de dados
     * contém o string <filename>. */
    unsigned int block_size, inode_area, inodes_per_block, records_per_block, ptrs_per_block;
    unsigned int lb, ri;
    struct sofs_inode root, inode;
    unsigned char *buf, *ibuf;
    int abs_block, found, new_inode, new_block;
    unsigned int dir_idx = 0;

    if (!g_mounted || linkname == NULL || filename == NULL)
        return -1;

    if (strlen(filename) > 50 || strlen(linkname) > 50)
        return -1;

    block_size = g_superbloco.blockSize * SECTOR_SIZE;
    inode_area = 1 + g_superbloco.freeBlocksBitmapSize + g_superbloco.freeInodeBitmapSize;
    inodes_per_block = block_size / sizeof(struct sofs_inode);
    records_per_block = block_size / sizeof(struct sofs_record);
    ptrs_per_block = block_size / sizeof(DWORD);
    buf = (unsigned char *)__builtin_alloca(block_size);
    ibuf = (unsigned char *)__builtin_alloca(block_size);

    if (read_block(inode_area, ibuf) != 0)
        return -1;
    memcpy(&root, ibuf, sizeof(root));

    found = 0;
    for (lb = 0; lb < root.blocksFileSize; lb++)
    {
        if (lb < 2)
        {
            if (root.dataPtr[lb] == 0)
                continue;
            abs_block = (int)root.dataPtr[lb];
        }
        else if (lb - 2 < ptrs_per_block)
        {
            if (root.singleIndPtr == 0)
                continue;
            if (read_block(root.singleIndPtr, buf) != 0)
                continue;
            if (((DWORD *)buf)[lb - 2] == 0)
                continue;
            abs_block = (int)((DWORD *)buf)[lb - 2];
        }
        else
            continue;

        if (read_block((unsigned int)abs_block, buf) != 0)
            continue;
        for (ri = 0; ri < records_per_block; ri++)
        {
            struct sofs_record *records = (struct sofs_record *)buf;
            if (records[ri].TypeVal != TYPEVAL_INVALIDO &&
                strcmp(records[ri].name, linkname) == 0)
            {
                found = 1;
                break;
            }
        }
        if (found)
            break;
    }

    if (found)
    {
        return -1;
    }

    new_inode = alloc_inode();
    if (new_inode < 0)
        return -1;

    new_block = alloc_data_block();
    if (new_block < 0)
    {
        free_inode(new_inode);
        return -1;
    }

    memset(buf, 0, block_size);
    strncpy((char *)buf, filename, 50);
    if (write_block((unsigned int)new_block, buf) != 0)
    {
        free_data_block(new_block);
        free_inode(new_inode);
        return -1;
    }

    memset(&inode, 0, sizeof(inode));
    inode.blocksFileSize = 1;
    inode.bytesFileSize = strlen(filename);
    inode.dataPtr[0] = (DWORD)new_block;
    inode.RefCounter = 1;

    if (read_block(inode_area + (unsigned int)new_inode / inodes_per_block, ibuf) != 0)
    {
        free_data_block(new_block);
        free_inode(new_inode);
        return -1;
    }
    memcpy(ibuf + ((unsigned int)new_inode % inodes_per_block) * sizeof(struct sofs_inode),
           &inode, sizeof(inode));
    if (write_block(inode_area + (unsigned int)new_inode / inodes_per_block, ibuf) != 0)
    {
        free_data_block(new_block);
        free_inode(new_inode);
        return -1;
    }

    abs_block = -1;
    for (lb = 0; lb < root.blocksFileSize; lb++)
    {
        int db;
        if (lb < 2)
        {
            if (root.dataPtr[lb] == 0)
                continue;
            db = (int)root.dataPtr[lb];
        }
        else if (lb - 2 < ptrs_per_block)
        {
            if (root.singleIndPtr == 0)
                continue;
            if (read_block(root.singleIndPtr, buf) != 0)
                continue;
            if (((DWORD *)buf)[lb - 2] == 0)
                continue;
            db = (int)((DWORD *)buf)[lb - 2];
        }
        else
            continue;
        if (read_block((unsigned int)db, buf) != 0)
            continue;
        for (ri = 0; ri < records_per_block; ri++)
        {
            if (((struct sofs_record *)buf)[ri].TypeVal == TYPEVAL_INVALIDO)
            {
                abs_block = db;
                dir_idx = ri;
                break;
            }
        }
        if (abs_block >= 0)
            break;
    }

    if (abs_block < 0)
    {
        unsigned int new_lb = root.blocksFileSize;
        if (new_lb < 2)
        {
            abs_block = alloc_data_block();
            if (abs_block < 0)
            {
                free_data_block(new_block);
                free_inode(new_inode);
                return -1;
            }
            root.dataPtr[new_lb] = (DWORD)abs_block;
            root.blocksFileSize++;
        }
        else if (new_lb - 2 < ptrs_per_block)
        {
            if (root.singleIndPtr == 0)
            {
                abs_block = alloc_data_block();
                if (abs_block < 0)
                {
                    free_data_block(new_block);
                    free_inode(new_inode);
                    return -1;
                }
                root.singleIndPtr = (DWORD)abs_block;
            }
            if (read_block(root.singleIndPtr, buf) != 0)
            {
                free_data_block(new_block);
                free_inode(new_inode);
                return -1;
            }
            if (((DWORD *)buf)[new_lb - 2] == 0)
            {
                abs_block = alloc_data_block();
                if (abs_block < 0)
                {
                    free_data_block(new_block);
                    free_inode(new_inode);
                    return -1;
                }
                ((DWORD *)buf)[new_lb - 2] = (DWORD)abs_block;
                if (write_block(root.singleIndPtr, buf) != 0)
                {
                    free_data_block(new_block);
                    free_inode(new_inode);
                    return -1;
                }
            }
            else
            {
                abs_block = (int)((DWORD *)buf)[new_lb - 2];
            }
            root.blocksFileSize++;
        }
        else
        {
            free_data_block(new_block);
            free_inode(new_inode);
            return -1;
        }
        if (read_block(inode_area, ibuf) != 0)
        {
            free_data_block(new_block);
            free_inode(new_inode);
            return -1;
        }
        memcpy(ibuf, &root, sizeof(root));
        if (write_block(inode_area, ibuf) != 0)
        {
            free_data_block(new_block);
            free_inode(new_inode);
            return -1;
        }
        dir_idx = 0;
        if (read_block((unsigned int)abs_block, buf) != 0)
        {
            free_data_block(new_block);
            free_inode(new_inode);
            return -1;
        }
    }

    memset(&((struct sofs_record *)buf)[dir_idx], 0, sizeof(struct sofs_record));
    ((struct sofs_record *)buf)[dir_idx].TypeVal = TYPEVAL_LINK;
    strncpy(((struct sofs_record *)buf)[dir_idx].name, linkname, 50);
    ((struct sofs_record *)buf)[dir_idx].name[50] = '\0';
    ((struct sofs_record *)buf)[dir_idx].inodeNumber = (DWORD)new_inode;
    if (write_block((unsigned int)abs_block, buf) != 0)
    {
        free_data_block(new_block);
        free_inode(new_inode);
        return -1;
    }

    return 0;
}

int sofs_hln(char *linkname, char *filename)
{
    /* TODO: cria um hardlink chamado <linkname> apontando para o mesmo
     * i-node que <filename>; incrementa o campo RefCounter do i-node. */
    unsigned int block_size, inode_area, inodes_per_block, records_per_block, ptrs_per_block;
    unsigned int lb, ri;
    struct sofs_inode root, inode;
    unsigned char *buf, *ibuf;
    int abs_block, found, target_found;
    unsigned int dir_idx = 0;
    unsigned int target_inode_num = 0;
    BYTE target_type = TYPEVAL_INVALIDO;

    if (!g_mounted || linkname == NULL || filename == NULL)
        return -1;

    if (strlen(filename) > 50 || strlen(linkname) > 50)
        return -1;

    block_size = g_superbloco.blockSize * SECTOR_SIZE;
    inode_area = 1 + g_superbloco.freeBlocksBitmapSize + g_superbloco.freeInodeBitmapSize;
    inodes_per_block = block_size / sizeof(struct sofs_inode);
    records_per_block = block_size / sizeof(struct sofs_record);
    ptrs_per_block = block_size / sizeof(DWORD);
    buf = (unsigned char *)__builtin_alloca(block_size);
    ibuf = (unsigned char *)__builtin_alloca(block_size);

    if (read_block(inode_area, ibuf) != 0)
        return -1;
    memcpy(&root, ibuf, sizeof(root));

    found = 0;
    target_found = 0;
    for (lb = 0; lb < root.blocksFileSize; lb++)
    {
        if (lb < 2)
        {
            if (root.dataPtr[lb] == 0)
                continue;
            abs_block = (int)root.dataPtr[lb];
        }
        else if (lb - 2 < ptrs_per_block)
        {
            if (root.singleIndPtr == 0)
                continue;
            if (read_block(root.singleIndPtr, buf) != 0)
                continue;
            if (((DWORD *)buf)[lb - 2] == 0)
                continue;
            abs_block = (int)((DWORD *)buf)[lb - 2];
        }
        else
            continue;

        if (read_block((unsigned int)abs_block, buf) != 0)
            continue;
        for (ri = 0; ri < records_per_block; ri++)
        {
            struct sofs_record *records = (struct sofs_record *)buf;
            if (records[ri].TypeVal != TYPEVAL_INVALIDO)
            {
                if (strcmp(records[ri].name, linkname) == 0)
                {
                    found = 1;
                }
                if (strcmp(records[ri].name, filename) == 0)
                {
                    target_found = 1;
                    target_inode_num = records[ri].inodeNumber;
                    target_type = records[ri].TypeVal;
                }
            }
        }
    }

    if (found)
    {
        return -1;
    }
    if (!target_found)
    {
        return -1;
    }

    if (read_block(inode_area + target_inode_num / inodes_per_block, ibuf) != 0)
        return -1;
    memcpy(&inode, ibuf + (target_inode_num % inodes_per_block) * sizeof(struct sofs_inode),
           sizeof(inode));

    inode.RefCounter++;

    memcpy(ibuf + (target_inode_num % inodes_per_block) * sizeof(struct sofs_inode),
           &inode, sizeof(inode));
    if (write_block(inode_area + target_inode_num / inodes_per_block, ibuf) != 0)
        return -1;

    if (read_block(inode_area, ibuf) != 0)
        return -1;
    memcpy(&root, ibuf, sizeof(root));

    for (lb = 0; lb < root.blocksFileSize; lb++)
    {
        int db;
        if (lb < 2)
        {
            if (root.dataPtr[lb] == 0)
                continue;
            db = (int)root.dataPtr[lb];
        }
        else if (lb - 2 < ptrs_per_block)
        {
            if (root.singleIndPtr == 0)
                continue;
            if (read_block(root.singleIndPtr, buf) != 0)
                continue;
            if (((DWORD *)buf)[lb - 2] == 0)
                continue;
            db = (int)((DWORD *)buf)[lb - 2];
        }
        else
            continue;
        if (read_block((unsigned int)db, buf) != 0)
            continue;
        for (ri = 0; ri < records_per_block; ri++)
        {
            if (((struct sofs_record *)buf)[ri].TypeVal == TYPEVAL_INVALIDO)
            {
                abs_block = db;
                dir_idx = ri;
                break;
            }
        }
        if (abs_block >= 0)
            break;
    }

    if (abs_block < 0)
    {
        unsigned int new_lb = root.blocksFileSize;
        if (new_lb < 2)
        {
            abs_block = alloc_data_block();
            if (abs_block < 0)
            {
                inode.RefCounter--;
                if (read_block(inode_area + target_inode_num / inodes_per_block, ibuf) == 0)
                {
                    memcpy(ibuf + (target_inode_num % inodes_per_block) * sizeof(struct sofs_inode),
                           &inode, sizeof(inode));
                    write_block(inode_area + target_inode_num / inodes_per_block, ibuf);
                }
                return -1;
            }
            root.dataPtr[new_lb] = (DWORD)abs_block;
            root.blocksFileSize++;
        }
        else if (new_lb - 2 < ptrs_per_block)
        {
            if (root.singleIndPtr == 0)
            {
                abs_block = alloc_data_block();
                if (abs_block < 0)
                {
                    inode.RefCounter--;
                    if (read_block(inode_area + target_inode_num / inodes_per_block, ibuf) == 0)
                    {
                        memcpy(ibuf + (target_inode_num % inodes_per_block) * sizeof(struct sofs_inode),
                               &inode, sizeof(inode));
                        write_block(inode_area + target_inode_num / inodes_per_block, ibuf);
                    }
                    return -1;
                }
                root.singleIndPtr = (DWORD)abs_block;
            }
            if (read_block(root.singleIndPtr, buf) != 0)
            {
                inode.RefCounter--;
                if (read_block(inode_area + target_inode_num / inodes_per_block, ibuf) == 0)
                {
                    memcpy(ibuf + (target_inode_num % inodes_per_block) * sizeof(struct sofs_inode),
                           &inode, sizeof(inode));
                    write_block(inode_area + target_inode_num / inodes_per_block, ibuf);
                }
                return -1;
            }
            if (((DWORD *)buf)[new_lb - 2] == 0)
            {
                abs_block = alloc_data_block();
                if (abs_block < 0)
                {
                    inode.RefCounter--;
                    if (read_block(inode_area + target_inode_num / inodes_per_block, ibuf) == 0)
                    {
                        memcpy(ibuf + (target_inode_num % inodes_per_block) * sizeof(struct sofs_inode),
                               &inode, sizeof(inode));
                        write_block(inode_area + target_inode_num / inodes_per_block, ibuf);
                    }
                    return -1;
                }
                ((DWORD *)buf)[new_lb - 2] = (DWORD)abs_block;
                if (write_block(root.singleIndPtr, buf) != 0)
                {
                    inode.RefCounter--;
                    if (read_block(inode_area + target_inode_num / inodes_per_block, ibuf) == 0)
                    {
                        memcpy(ibuf + (target_inode_num % inodes_per_block) * sizeof(struct sofs_inode),
                               &inode, sizeof(inode));
                        write_block(inode_area + target_inode_num / inodes_per_block, ibuf);
                    }
                    return -1;
                }
            }
            else
            {
                abs_block = (int)((DWORD *)buf)[new_lb - 2];
            }
            root.blocksFileSize++;
        }
        else
        {
            inode.RefCounter--;
            if (read_block(inode_area + target_inode_num / inodes_per_block, ibuf) == 0)
            {
                memcpy(ibuf + (target_inode_num % inodes_per_block) * sizeof(struct sofs_inode),
                       &inode, sizeof(inode));
                write_block(inode_area + target_inode_num / inodes_per_block, ibuf);
            }
            return -1;
        }
        if (read_block(inode_area, ibuf) != 0)
        {
            return -1;
        }
        memcpy(ibuf, &root, sizeof(root));
        if (write_block(inode_area, ibuf) != 0)
        {
            return -1;
        }
        dir_idx = 0;
        if (read_block((unsigned int)abs_block, buf) != 0)
        {
            return -1;
        }
    }

    memset(&((struct sofs_record *)buf)[dir_idx], 0, sizeof(struct sofs_record));
    ((struct sofs_record *)buf)[dir_idx].TypeVal = target_type;
    strncpy(((struct sofs_record *)buf)[dir_idx].name, linkname, 50);
    ((struct sofs_record *)buf)[dir_idx].name[50] = '\0';
    ((struct sofs_record *)buf)[dir_idx].inodeNumber = target_inode_num;
    if (write_block((unsigned int)abs_block, buf) != 0)
    {
        return -1;
    }

    return 0;
}
