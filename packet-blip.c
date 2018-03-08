// Experimental dissector for BLIP (https://github.com/couchbaselabs/BLIP-Cpp)
//
// License: Apache2
//
// BLIP protocol spec: https://github.com/couchbaselabs/BLIP-Cpp/blob/a33262740787bbdfb17eef6d8a6ab4a5e18fe089/docs/BLIP%20Protocol.md
//

#include "config.h"

#include <epan/packet.h>
#include <epan/tvbparse.h>
#include <wsutil/wsjsmn.h>

#include <wsutil/str_util.h>
#include <wsutil/unicode-utils.h>

#include <wiretap/wtap.h>
#include <printf.h>

#include "packet-http.h"


// Cribbed from https://stackoverflow.com/questions/111928/is-there-a-printf-converter-to-print-in-binary-format
#define PRINTF_BINARY_PATTERN_INT8 "%c%c%c%c%c%c%c%c"
#define PRINTF_BYTE_TO_BINARY_INT8(i)    \
    (((i) & 0x80ll) ? '1' : '0'), \
    (((i) & 0x40ll) ? '1' : '0'), \
    (((i) & 0x20ll) ? '1' : '0'), \
    (((i) & 0x10ll) ? '1' : '0'), \
    (((i) & 0x08ll) ? '1' : '0'), \
    (((i) & 0x04ll) ? '1' : '0'), \
    (((i) & 0x02ll) ? '1' : '0'), \
    (((i) & 0x01ll) ? '1' : '0')

#define PRINTF_BINARY_PATTERN_INT16 \
    PRINTF_BINARY_PATTERN_INT8              PRINTF_BINARY_PATTERN_INT8
#define PRINTF_BYTE_TO_BINARY_INT16(i) \
    PRINTF_BYTE_TO_BINARY_INT8((i) >> 8),   PRINTF_BYTE_TO_BINARY_INT8(i)
#define PRINTF_BINARY_PATTERN_INT32 \
    PRINTF_BINARY_PATTERN_INT16             PRINTF_BINARY_PATTERN_INT16
#define PRINTF_BYTE_TO_BINARY_INT32(i) \
    PRINTF_BYTE_TO_BINARY_INT16((i) >> 16), PRINTF_BYTE_TO_BINARY_INT16(i)
#define PRINTF_BINARY_PATTERN_INT64    \
    PRINTF_BINARY_PATTERN_INT32             PRINTF_BINARY_PATTERN_INT32
#define PRINTF_BYTE_TO_BINARY_INT64(i) \
    PRINTF_BYTE_TO_BINARY_INT32((i) >> 32), PRINTF_BYTE_TO_BINARY_INT32(i)
/* --- end macros --- */


static dissector_handle_t blip_handle;

static int proto_blip = -1;

static int hf_blip_message_number = -1;
static int hf_blip_frame_flags = -1;
static int hf_blip_properties_length = -1;
static int hf_blip_properties = -1;

static gint ett_blip = -1;


static int
dissect_blip(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void *data)
{

    proto_tree *blip_tree;
    gint        offset = 0;

    /* Set the protcol column to say BLIP */
    col_set_str(pinfo->cinfo, COL_PROTOCOL, "BLIP");

    /* Clear out stuff in the info column */
    col_clear(pinfo->cinfo,COL_INFO);

    // ------------------------------------- BLIP tree -----------------------------------------------------------------


    /* Add a subtree to dissection.  See 9.2.2. Dissecting the details of the protocol of WSDG */
    proto_item *blip_item = proto_tree_add_item(tree, proto_blip, tvb, offset, -1, ENC_NA);

    blip_tree = proto_item_add_subtree(blip_item, ett_blip);


    // ------------------------ BLIP Frame Header: Message Number VarInt -----------------------------------------------

    // This gets the message number as a var int in order to find out how much to bump
    // the offset for the next proto_tree item
    guint64 value_message_num;
    guint varint_message_num_length = tvb_get_varint(
            tvb,
            offset,
            FT_VARINT_MAX_LEN,
            &value_message_num,
            ENC_VARINT_PROTOBUF);

    printf("BLIP message number: %" G_GUINT64_FORMAT "\n", value_message_num);

    proto_tree_add_item(blip_tree, hf_blip_message_number, tvb, offset, varint_message_num_length, ENC_VARINT_PROTOBUF);

    offset += varint_message_num_length;
    printf("new offset: %d\n", offset);


    // ------------------------ BLIP Frame Header: Frame Flags VarInt --------------------------------------------------

    // This gets the message number as a var int in order to find out how much to bump
    // the offset for the next proto_tree item
    guint64 value_frame_flags;
    guint varint_frame_flags_length = tvb_get_varint(
            tvb,
            offset,
            FT_VARINT_MAX_LEN,
            &value_frame_flags,
            ENC_VARINT_PROTOBUF);

    printf("BLIP frame flags: %" G_GUINT64_FORMAT "\n", value_frame_flags);

    proto_tree_add_item(blip_tree, hf_blip_frame_flags, tvb, offset, varint_frame_flags_length, ENC_VARINT_PROTOBUF);

    offset += varint_frame_flags_length;
    printf("new offset: %d\n", offset);

    printf("Frame flags "
                   PRINTF_BINARY_PATTERN_INT8 "\n",
           PRINTF_BYTE_TO_BINARY_INT8(value_frame_flags));


    // TODO: if this flag is set:
    // TODO:    MoreComing= 0x40  // 0100 0000
    // TODO: it should issue warnings that subsequent packets in this conversation will be broken, since it currently
    // TODO: doesn't handle messages split among multiple frames

    // TODO: if this flag is set:
    // TODO:    Compressed= 0x08  // 0000 1000
    // TODO: it should not try to decode the body into json (or are properties compressed too!?)

    // ------------------------ BLIP Frame Header: Properties Length VarInt --------------------------------------------------

    // WARNING: this only works because this code assumes that ALL MESSAGES FIT INTO ONE FRAME, which is absolutely not true.
    // In other words, as soon as there is a message that spans two frames, this code will break.

    guint64 value_properties_length;
    guint value_properties_length_varint_length = tvb_get_varint(
            tvb,
            offset,
            FT_VARINT_MAX_LEN,
            &value_properties_length,
            ENC_VARINT_PROTOBUF);

    printf("BLIP properties length: %" G_GUINT64_FORMAT "\n", value_properties_length);

    proto_tree_add_item(blip_tree, hf_blip_properties_length, tvb, offset, value_properties_length_varint_length, ENC_VARINT_PROTOBUF);

    offset += value_properties_length_varint_length;
    printf("new offset: %d\n", offset);



    // ------------------------ BLIP Frame: Properties --------------------------------------------------

    // WARNING: this only works because this code assumes that ALL MESSAGES FIT INTO ONE FRAME, which is absolutely not true.
    // In other words, as soon as there is a message that spans two frames, this code will break.

    // ENC_UTF_8

    const guint8* buf = tvb_get_string_enc(wmem_packet_scope(), tvb, offset, (gint) value_properties_length, ENC_UTF_8);
    printf("buf: %s\n", buf);

    // original
    // proto_tree_add_item(blip_tree, hf_blip_properties, tvb, offset, (gint) value_properties_length, ENC_UTF_8);

    //     char buf[] = "Profile\0subChanges\0continuous\0true\0foo\0\bar";


    int string_offset = offset;
    proto_tree_add_item(blip_tree, hf_blip_properties, tvb, string_offset, (gint) 7, ENC_UTF_8);
    string_offset += 7;  // "Profile"
    string_offset += 1;  // \0

    proto_tree_add_item(blip_tree, hf_blip_properties, tvb, string_offset, (gint) 10, ENC_UTF_8);
    string_offset += 10;  // "subChanges"
    string_offset += 1;  // \0

    proto_tree_add_item(blip_tree, hf_blip_properties, tvb, string_offset, (gint) 10, ENC_UTF_8);
    string_offset += 10;  // "continuous"
    string_offset += 1;  // \0

    proto_tree_add_item(blip_tree, hf_blip_properties, tvb, string_offset, (gint) 4, ENC_UTF_8);
    string_offset += 4;  // "true"
    string_offset += 1;  // \0

    proto_tree_add_item(blip_tree, hf_blip_properties, tvb, string_offset, (gint) 3, ENC_UTF_8);
    string_offset += 3;  // "foo"
    string_offset += 1;  // \0

    proto_tree_add_item(blip_tree, hf_blip_properties, tvb, string_offset, (gint) 3, ENC_UTF_8);
    string_offset += 3;  // "bar"
    string_offset += 1;  // \0


//    proto_tree_add_item(blip_tree, hf_blip_properties, tvb, offset+7, (gint) 10, ENC_UTF_8);
//    proto_tree_add_item(blip_tree, hf_blip_properties, tvb, offset+17, (gint) 10, ENC_UTF_8);

//    int props_offset = offset;
//    for (int i = 0; i < value_properties_length; i++) {
//        if (buf[i] == '\0') {
//
//        }
//    }


    offset += value_properties_length;
    printf("new offset: %d\n", offset);



    // -------------------------------------------- Etc ----------------------------------------------------------------

    // Stop compiler from complaining about unused variables
    if (pinfo || tree || data) {

    }

    return tvb_captured_length(tvb);
}




void
proto_register_blip(void)
{

    static hf_register_info hf[] = {
            { &hf_blip_message_number,
                    { "BLIP Message Number", "blip.messagenum",
                            FT_UINT64, BASE_DEC,
                            NULL, 0x0,
                            NULL, HFILL }
            },
            { &hf_blip_frame_flags,
                    { "BLIP Frame Flags", "blip.frameflags",
                            FT_UINT64, BASE_DEC,
                            NULL, 0x0,
                            NULL, HFILL }
            },
            { &hf_blip_properties_length,
                    { "BLIP Properties Length", "blip.propslength",
                            FT_UINT64, BASE_DEC,
                            NULL, 0x0,
                            NULL, HFILL }
            },
            { &hf_blip_properties,
                    { "BLIP Properties", "blip.props",
                            FT_STRING, STR_UNICODE,
                            NULL, 0x0,
                            NULL, HFILL }
            },
    };

    /* Setup protocol subtree array */
    static gint *ett[] = {
            &ett_blip
    };

    proto_blip = proto_register_protocol("BLIP Couchbase Mobile", "BLIP", "blip");

    proto_register_field_array(proto_blip, hf, array_length(hf));
    proto_register_subtree_array(ett, array_length(ett));

    blip_handle = register_dissector("blip", dissect_blip, proto_blip);

}

void
proto_reg_handoff_blip(void)
{

    // Register the blip dissector as a subprotocol dissector of "ws.protocol",
    // matching any packets with a Web-Sec-Protocol header of "BLIP_3+CBMobile_2".
    //
    // See https://github.com/couchbase/sync_gateway/issues/3356#issuecomment-370958321 for
    // more notes on how the websocket dissector routes packets down to subprotocol handlers.

    ftenum_t type;
    dissector_table_t table = find_dissector_table("ws.protocol");
    if (table) {
        //printf("table is not nil");
    }
    type = get_dissector_table_selector_type("ws.protocol");
    if (type == FT_STRING) {
        // printf("is FT_STRING");
        dissector_add_string("ws.protocol", "BLIP_3+CBMobile_2", blip_handle);
    } else {
        // printf("not FT_STRING");
    }


}
