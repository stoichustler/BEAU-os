// SPDX-License-Identifier: GPL-2.0-only
/*
 * BEAU VM0 <-> VM3 RPMsg peer service.
 *
 * The service binds a dedicated endpoint and leaves rpmsg-raw to the existing
 * rpmsg_char smoke utility. READY publishes the Linux endpoint to VM0; every
 * validated DATA request is acknowledged with the same sequence and payload.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/rpmsg.h>
#include <linux/unaligned.h>

#define BEAU_RPMSG_PEER_NAME		"beau-rpmsg-peer"
#define BEAU_RPMSG_PEER_MAGIC		0x4252504dU
#define BEAU_RPMSG_PEER_VERSION	1U
#define BEAU_RPMSG_PEER_READY		1U
#define BEAU_RPMSG_PEER_DATA		2U
#define BEAU_RPMSG_PEER_ACK		3U
#define BEAU_RPMSG_PEER_PAYLOAD_MAX	240U

struct beau_rpmsg_peer_msg {
	__le32 magic;
	__le16 version;
	__le16 type;
	__le32 sequence;
	__le16 payload_len;
	__le16 reserved;
	u8 payload[BEAU_RPMSG_PEER_PAYLOAD_MAX];
} __packed;

struct beau_rpmsg_peer_packet {
	u16 type;
	u32 sequence;
	u16 payload_len;
	const u8 *payload;
};

static size_t beau_rpmsg_peer_header_size(void)
{
	return offsetof(struct beau_rpmsg_peer_msg, payload);
}

static int beau_rpmsg_peer_decode(const void *data, int len,
				  struct beau_rpmsg_peer_packet *packet)
{
	const struct beau_rpmsg_peer_msg *msg = data;
	size_t header_size = beau_rpmsg_peer_header_size();
	u16 payload_len;

	if (!data || !packet || len < 0 || (size_t)len < header_size)
		return -EINVAL;
	payload_len = get_unaligned_le16(&msg->payload_len);
	if (get_unaligned_le32(&msg->magic) != BEAU_RPMSG_PEER_MAGIC ||
		get_unaligned_le16(&msg->version) != BEAU_RPMSG_PEER_VERSION ||
		get_unaligned_le16(&msg->reserved) != 0U ||
		payload_len > BEAU_RPMSG_PEER_PAYLOAD_MAX ||
		len != (int)(header_size + payload_len))
		return -EINVAL;

	packet->type = get_unaligned_le16(&msg->type);
	packet->sequence = get_unaligned_le32(&msg->sequence);
	packet->payload_len = payload_len;
	packet->payload = msg->payload;
	return 0;
}

static int beau_rpmsg_peer_send(struct rpmsg_device *rpdev, u16 type,
				u32 sequence, const u8 *payload, u16 payload_len)
{
	struct beau_rpmsg_peer_msg msg = { };
	size_t len = beau_rpmsg_peer_header_size() + payload_len;

	if (payload_len > BEAU_RPMSG_PEER_PAYLOAD_MAX ||
		(payload_len != 0U && !payload))
		return -EINVAL;
	msg.magic = cpu_to_le32(BEAU_RPMSG_PEER_MAGIC);
	msg.version = cpu_to_le16(BEAU_RPMSG_PEER_VERSION);
	msg.type = cpu_to_le16(type);
	msg.sequence = cpu_to_le32(sequence);
	msg.payload_len = cpu_to_le16(payload_len);
	if (payload_len != 0U)
		memcpy(msg.payload, payload, payload_len);
	return rpmsg_send(rpdev->ept, &msg, len);
}

/* [20260723] Dedicated VM0 peer protocol
 *
 * Linux probe -> READY -> VM0 records local endpoint address
 * VM0 DATA    -> validate -> Linux ACK with identical sequence and payload
 *
 * Key rule:
 *   - this driver owns only beau-rpmsg-peer and never consumes rpmsg-raw;
 *   - validate the complete bounded packet before logging or replying;
 *   - failed validation produces no reply, so VM0 cannot mistake it for ACK.
 */
static int beau_rpmsg_peer_callback(struct rpmsg_device *rpdev, void *data,
				     int len, void *priv, u32 src)
{
	struct beau_rpmsg_peer_packet packet;
	int ret;

	(void)priv;
	if (beau_rpmsg_peer_decode(data, len, &packet)) {
		dev_warn_ratelimited(&rpdev->dev, "invalid packet: src=0x%x len=%d\n",
			src, len);
		return 0;
	}
	if (packet.type != BEAU_RPMSG_PEER_DATA) {
		dev_warn_ratelimited(&rpdev->dev, "unexpected type=%u seq=%u src=0x%x\n",
			packet.type, packet.sequence, src);
		return 0;
	}

	dev_info(&rpdev->dev, "rx seq=%u len=%u payload=%.*s\n", packet.sequence,
		 packet.payload_len, packet.payload_len, packet.payload);
	ret = beau_rpmsg_peer_send(rpdev, BEAU_RPMSG_PEER_ACK, packet.sequence,
		packet.payload, packet.payload_len);
	if (ret)
		dev_err(&rpdev->dev, "ACK failed: seq=%u ret=%d\n", packet.sequence, ret);
	return 0;
}

static int beau_rpmsg_peer_probe(struct rpmsg_device *rpdev)
{
	int ret;

	ret = beau_rpmsg_peer_send(rpdev, BEAU_RPMSG_PEER_READY, 0U, NULL, 0U);
	if (ret)
		return dev_err_probe(&rpdev->dev, ret, "READY failed\n");
	dev_info(&rpdev->dev, "peer ready: local=0x%x remote=0x%x\n",
		rpdev->src, rpdev->dst);
	return 0;
}

static const struct rpmsg_device_id beau_rpmsg_peer_id_table[] = {
	{ .name = BEAU_RPMSG_PEER_NAME },
	{ },
};
MODULE_DEVICE_TABLE(rpmsg, beau_rpmsg_peer_id_table);

static struct rpmsg_driver beau_rpmsg_peer_driver = {
	.drv.name = "beau-rpmsg-peer",
	.id_table = beau_rpmsg_peer_id_table,
	.probe = beau_rpmsg_peer_probe,
	.callback = beau_rpmsg_peer_callback,
};
module_rpmsg_driver(beau_rpmsg_peer_driver);

MODULE_DESCRIPTION("BEAU VM0 VM3 full-duplex RPMsg peer");
MODULE_LICENSE("GPL");
