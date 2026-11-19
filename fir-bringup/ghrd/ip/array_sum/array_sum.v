// Sum an array of unsigned 32-bit numbers in HPS DDR memory.
// Registers 0–3 mimic the Intel PIO layout for the Linux GPIO driver.

module array_sum (
    input  wire        clk,
    input  wire        reset,

    // Avalon-MM register interface, word addressed
    input  wire [3:0]  avs_address,
    input  wire        avs_read,
    output reg  [31:0] avs_readdata,
    input  wire        avs_write,
    input  wire [31:0] avs_writedata,

    // Avalon-MM master for burst reads from HPS memory
    output reg  [31:0] avm_address,
    output reg  [4:0]  avm_burstcount,
    output reg         avm_read,
    input  wire        avm_waitrequest,
    input  wire [63:0] avm_readdata,
    input  wire        avm_readdatavalid,

    output wire        irq
);

    localparam [31:0] ID_VALUE = 32'h53554D31;
    localparam [4:0]  MAX_BURST = 5'd16;

    reg [31:0] base_addr;
    reg [31:0] count;
    reg [31:0] beats_to_issue;
    reg [31:0] beats_to_recv;
    reg [63:0] sum;
    reg [31:0] cycles;
    reg        busy;
    reg        done;
    reg        irq_mask;
    reg        edge_cap;

    wire        start = avs_write && (avs_address == 4'd4)
                        && avs_writedata[0] && !busy;
    wire [31:0] total_beats = (count >> 1) + {31'd0, count[0]};
    wire [31:0] issue_left = beats_to_issue - {27'd0, avm_burstcount};
    wire        last_beat = (beats_to_recv == 32'd1);

    // An odd count leaves only the lower word valid in the final beat.
    wire [63:0] beat_value = (last_beat && count[0])
                            ? {32'd0, avm_readdata[31:0]}
                            : {32'd0, avm_readdata[31:0]}
                            + {32'd0, avm_readdata[63:32]};

    assign irq = edge_cap & irq_mask;

    always @(posedge clk) begin
        if (reset) begin
            base_addr      <= 32'd0;
            count          <= 32'd0;
            beats_to_issue <= 32'd0;
            beats_to_recv  <= 32'd0;
            sum            <= 64'd0;
            cycles         <= 32'd0;
            busy           <= 1'b0;
            done           <= 1'b0;
            irq_mask       <= 1'b0;
            edge_cap       <= 1'b0;
            avm_address    <= 32'd0;
            avm_burstcount <= 5'd0;
            avm_read       <= 1'b0;
        end else begin
            if (avs_write) begin
                case (avs_address)
                    4'd2: irq_mask <= avs_writedata[0];
                    4'd3: if (avs_writedata[0]) edge_cap <= 1'b0;
                    4'd6: if (!busy) base_addr <= avs_writedata;
                    4'd7: if (!busy) count <= avs_writedata;
                    default: ;
                endcase
            end

            if (start) begin
                sum      <= 64'd0;
                cycles   <= 32'd0;
                done     <= 1'b0;
                edge_cap <= 1'b0;

                if (count == 32'd0) begin
                    done     <= 1'b1;
                    edge_cap <= 1'b1;
                end else begin
                    busy           <= 1'b1;
                    beats_to_issue <= total_beats;
                    beats_to_recv  <= total_beats;
                    avm_address    <= {base_addr[31:3], 3'b000};
                    avm_burstcount <= (total_beats > MAX_BURST)
                                      ? MAX_BURST : total_beats[4:0];
                    avm_read       <= 1'b1;
                end
            end else if (busy) begin
                cycles <= cycles + 32'd1;

                // Advance to the next burst once the current request is accepted.
                if (avm_read && !avm_waitrequest) begin
                    beats_to_issue <= issue_left;
                    avm_address    <= avm_address
                                      + {24'd0, avm_burstcount, 3'b000};
                    if (issue_left == 32'd0)
                        avm_read <= 1'b0;
                    else
                        avm_burstcount <= (issue_left > MAX_BURST)
                                          ? MAX_BURST : issue_left[4:0];
                end

                // Add each returned beat, then signal completion on the last one.
                if (avm_readdatavalid) begin
                    sum           <= sum + beat_value;
                    beats_to_recv <= beats_to_recv - 32'd1;
                    if (last_beat) begin
                        busy     <= 1'b0;
                        done     <= 1'b1;
                        edge_cap <= 1'b1;
                    end
                end
            end
        end
    end

    always @(*) begin
        case (avs_address)
            4'd0:    avs_readdata = {31'd0, done};
            4'd2:    avs_readdata = {31'd0, irq_mask};
            4'd3:    avs_readdata = {31'd0, edge_cap};
            4'd5:    avs_readdata = {30'd0, done, busy};
            4'd6:    avs_readdata = base_addr;
            4'd7:    avs_readdata = count;
            4'd8:    avs_readdata = sum[31:0];
            4'd9:    avs_readdata = sum[63:32];
            4'd10:   avs_readdata = cycles;
            4'd11:   avs_readdata = ID_VALUE;
            default: avs_readdata = 32'd0;
        endcase
    end

endmodule
